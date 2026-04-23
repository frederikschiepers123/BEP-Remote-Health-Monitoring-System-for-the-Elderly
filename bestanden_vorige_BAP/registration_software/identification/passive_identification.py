from __future__ import annotations
import json, math, secrets, cv2, time, requests, numpy as np
from registration_software.identification.processing.calibrate import calibrate
from registration_software.identification.processing.qr_detector import QrCodeDetector  # returns label, center, bbox, authenticity
from registration_software.registration.registry import DeviceRegistry
from registration_software.interface.class_onvif import Class_onvif
from registration_software.common.config import *


class Cam_register():
    def __init__(self,
                 one_cam=False,
                 change_password=False,
                 password= None,
                 save_results=False,
                 ):
        self.onvif = Class_onvif()
        self.detect = QrCodeDetector()

        self.one_cam = one_cam
        self.save_results = save_results
        self.size_qr = SIZE_QR
        self.change_password = change_password
        self.use_y = USE_Y
        self.dis = DISTANCE
        self.password = password

        self.W1 = PIXELS_WIDTH_1
        self.H1 = PIXELS_HEIGHT_1
        self.HFOV1 = HORIZONTAL_FOV_ANGLE_1
        self.VFOV1 = VERTICAL_FOV_ANGLE_1

        self.W2 = PIXELS_WIDTH_2
        self.H2 = PIXELS_HEIGHT_2
        self.HFOV2 = HORIZONTAL_FOV_ANGLE_2
        self.VFOV2 = VERTICAL_FOV_ANGLE_2

        self.main_results = []
        self.secondary_results = []
        self.main_tvecs = []
        self.devices = []
        self.main_cam = {}
        self.main_label = None

    def start(self):
        # ----- Look for onvif devices on the lan -----
        self.devices = self.onvif.discover_onvif_devices()

        if len(self.devices) <= 1:
            print(f"Found {len(self.devices)} devices on the network.")
            print(f"Not enough cameras on the network.")
            return
        print(f"Found {len(self.devices)} devices on the network.")

        # ------ Check if MAIN_CAMERA_HOST is known ------
        # ------ Calibrate camera to ensure QR codes are readable -----
        if MAIN_CAMERA_HOST is not None:
                for d in self.devices:
                    if d["host"] == MAIN_CAMERA_HOST:
                        print(f'MAIN_CAMERA_HOST Found')
                        calibrate(device=d, user=USERNAME, password=PASSWORD, detector=self.detect)
                        self.main_cam = d
                        self.devices.remove(d)
        # -----If main camera host is known we go to a different registering program -------
                        self.reg_main_cam()
        # ----------------------------------------------------------------------------------
                        return
                    else:
                        continue
                print(f"MAIN_CAMERA_HOST: {MAIN_CAMERA_HOST} not found.")
                print(f'Starting calibration using first devices on the list.')
        calibrate(device=self.devices[0], user=USERNAME, password=PASSWORD, detector=self.detect)

        main = 0
        for d in self.devices[:]:
            # ----- Scan a frame from every camera for QR codes -----
            # Try to get a frame using the RTSP URL. Revert back to Snapshot URL to grab frame
            if d["snapshot_url"] == "":
                print(f"\nCamera {d['host']} does not have a Snapshot URL")
                print(f"Trying RTSP URL")
                if d["rtsp_url"] == "":
                    print(f"\nCamera {d['host']} also does not have an RTSP URL")
                    print(f"\nSkipping camera {d['host']}")
                    continue
                else:
                    frame = self.onvif.get_frame(d["snapshot_url"])
            else:
                frames = self.onvif.capture_x_second(d["rtsp_url"])
                frame = frames[-1] if len(frames)==1 else frames[-2]

            # ----- Scan the frame for QR codes -----
            results = self.detect.detect_single_frame(frame)

            # -----Can save results and scanned frame to a file for testing/debugging-----
            if self.save_results:
                save_result_frame(results, frame, d["host"])

            if not results:
                print(f"\nNo QR codes detected in FOV of {d['host']}.")
                self.devices.remove(d)
                continue
            print(f"Found {len(results)} QR codes in the FOV of {d['host']}.")

            # ----- Filter results for authenticity -----
            results = [r for r in results if r.get("authenticity") is True]
            print(f"After discarding unauthentic and unreadable QR codes {len(results)} QR codes are left.\n")
            if len(results) == 0:
                print(f"No Valid QR codes detected in FOV of {d['host']}.\n")
                self.devices.remove(d)
                continue

            # ----- Get Translation of all cameras relative to QR codes in their FOV -----
            if self.one_cam:
                quad = results[0]["quad"]
                tvec = self.compute_tvec(self.W2, self.H2, self.HFOV2, self.VFOV2, quad2=quad)
                d["translation"] = tvec
                d["results"] = results
                self.secondary_results.append(results[0])
            else:
                if len(results) >= 2:
                    for r in results:
                        quad = r["quad"]
                        tvec = self.compute_tvec(self.W1, self.H1, self.HFOV1, self.VFOV1, quad2=quad)
                        r["translation"] = tvec
                        self.main_tvecs.append(np.asarray(tvec, dtype=float).reshape(3, ))
                    self.main_results = results
                    self.main_cam = d
                    self.devices.remove(d)
                    main = main + 1
                else:
                    quad = results[0]["quad"]
                    tvec = self.compute_tvec(self.W2, self.H2, self.HFOV2, self.VFOV2, quad2=quad)
                    d["translation"] = tvec
                    d["results"] = results
                    self.secondary_results.append(results[0])

        # Transfer one of the results to main cam if only using one secondary camera
        if self.one_cam:
            self.main_results = [self.secondary_results[0]]
            self.main_tvecs = self.devices[0]["translation"]
            self.secondary_results.pop(0)
            self.devices.pop(0)

        if main > 1:
            print(f'More than one devices has two authentic QR codes in it\'s FOV.')
            print(f'Could not know with camera is the main camera.\n')
            print(f'Fill in MAIN_CAMERA_HOST in the config,')
            print(f'or make sure only the main camera sees more than one authentic QR code.')
            return

        # ----- From here knowing or not knowing the ------------------
        # ----- MAIN_HOST_ADDRESS does not matter anymore -------------
        # ----- So we merge start() en reg_main_cam() back together ---
        self.merge()


    def reg_main_cam(self):
        """
        Function to register cameras when MAIN_CAMERA_HOST is known.
        """
        # ----- Scan frame from main cam for results -----
        if self.main_cam["snapshot_url"] == "":  # Try to get a frame using the snapshot URL. Revert back to RTSP URL to grab frame
            print(f"\nCamera {MAIN_CAMERA_HOST} also does not have an RTSP URL")
            print(f"Trying snapshot URL")
            if self.main_cam["rtsp_url"] == "":
                print(f"\nCamera {MAIN_CAMERA_HOST} does not have a Snapshot URL")
                return
            else:
                frame = self.onvif.get_frame(self.main_cam["snapshot_url"])
        else:
            frames = self.onvif.capture_x_second(self.main_cam["rtsp_url"])
            frame = frames[-1]

        self.main_results = self.detect.detect_single_frame(frame)

        # -----Can save results and scanned frame to a file for testing/debugging-----
        if self.save_results:
           save_result_frame(self.main_results, frame, self.main_cam["host"])

        # ----- Filter results for authenticity -----
        self.main_results = [r for r in self.main_results if r.get("authenticity") is True]

        print(f"After discarding unauthentic and unreadable QR codes {len(self.main_results)} QR codes are left.\n")
        if len(self.main_results) == 0:
            print("Not enough QR codes visible in Main camera FOV.")
            print("Make sure at least 1 QR code is visible in the main FOV.\n")
            return

        # ----- Get translations from main camera to all the authentic QR codes in its FOV
        for r in self.main_results:
            quad = r["quad"]
            tvec = self.compute_tvec(self.W1, self.H1, self.HFOV1, self.VFOV1, quad2=quad)
            r["translation"] = tvec
            self.main_tvecs.append(np.asarray(tvec, dtype=float).reshape(3, ))

        # ----- Scan a frame from every secondary camera for QR codes -----
        # Try to get a frame using the RTSP URL. Revert back to Snapshot URL to grab frame
        for d in self.devices[:]:
            if d["snapshot_url"] == "":
                print(f"\nCamera {d['host']} does not have a Snapshot URL")
                print(f"Trying RTSP URL")
                if d["rtsp_url"] == "":
                    print(f"\nCamera {d['host']} also does not have an RTSP URL")
                    print(f"\nSkipping camera {d['host']}")
                    continue
                else:
                    frame = self.onvif.get_frame(d["snapshot_url"])
            else:
                frames = self.onvif.capture_x_second(d["rtsp_url"])
                frame = frames[-1]

            # ----- Scan frame for QR codes -----
            results = self.detect.detect_single_frame(frame)

            # -----Can save results and scanned frame to a file for testing/debugging-----
            if self.save_results:
                save_result_frame(results, frame, d["host"])

            if not results:
                print(f"\nNo QR codes detected in FOV of {d['host']}.")
                self.devices.remove(d)
                continue
            print(f"Found {len(results)} QR codes in the FOV of {d['host']}.")

            # ----- Filter results for authenticity -----
            results = [r for r in results if r.get("authenticity") is True]
            print(f"After discarding unauthentic and unreadable QR codes {len(results)} QR codes are left.\n")
            if len(results) == 0:
                print(f"\nNo Valid QR codes detected in FOV of {d['host']}.")
                self.devices.remove(d)
                continue
            elif len(results) > 1:
                print(f'More then one authentic QR code seen in the FOV of {d["host"]}')
                print(f'Make sure only one valid QR code is visible in the secondary camera\'s FOV')
                print(f'Device will not be registered.')
                self.devices.remove(d)

            # ----- Get Translation of all cameras relative to QR codes in their FOV -----
            quad = results[0]["quad"]
            tvec = self.compute_tvec(self.W2, self.H2, self.HFOV2, self.VFOV2, quad2=quad)
            d["translation"] = tvec
            d["results"] = results
            self.secondary_results.append(results[0])

        # ----- From here knowing or not knowing the ------------------
        # ----- MAIN_HOST_ADDRESS does not matter anymore -------------
        # ----- So we merge start() en reg_main_cam() back together ---
        self.merge()

    def merge(self):
        # Check for results in main FOV
        if self.validate_results():
            return

        # ----- Determine label of main camera by comparing results from secondary cameras -----
        self.compare_secondary_results()

        # ----- Determining label of secondary cameras by comparing translations -----
        for d in self.devices:
            i, dis = self.translation_dis(t_cam=d["translation"])
            print(f"camera {d['host']} is closet to QR {self.main_results[i]['label']} with distance {dis}.\n")
            # print(f"Main tvecs : {self.main_tvecs}\n")
            # print(f'secondary tvec: {d["translation"]}\n')
            label = self.main_results[i]["label"]
            if dis < self.dis:
                d["label"] = label
            else:
                print(f'Distance between translation vector and closest main translation vector exceeds maximum')
                print(f'No label assigned to camera {d["host"]}\n')

        # Add main cam back to the devices if it was given a label so that it will also be registered.
        if self.main_label is not None:
            self.devices.append(self.main_cam)

        # ----- Write new devices to registry------
        self.write_to_registry()

    def validate_results(self):
        if self.one_cam:
            if len(self.secondary_results) < 1:
                print("Not enough QR codes detected from secondary camera FOV.")
                print("Make sure at least 1 QR code are visible in the main FOV.\n")
                return True
        else:
            if len(self.main_results) < 2:
                print("Not enough QR codes visible in Main camera FOV.")
                print("Make sure at least 2 QR codes are visible in the main FOV.\n")
                return True
            if len(self.secondary_results) < 1:
                print("Not enough QR codes detected from secondary cameras FOV.")
                print("Make sure at least 1 QR code are visible in the secondary FOV.\n")
                return True

    def compare_secondary_results(self):
        if self.secondary_results != []:
            main_label = self.secondary_results[0]["label"]
            for n in range(len(self.secondary_results)):
                next_label = self.secondary_results[n]["label"]
                if next_label == main_label:
                    self.main_label = next_label
                    self.main_cam["label"] = self.main_label
                    continue
                else:
                    self.main_label = None
                    print(f'Not every secondary camara sees the same QR code in their FOV.')
                    print(f'So the main camera could not be given a label with confidence.\n')
                    break
        else:
            print(f'Did not get any results from secondary cameras.')
            print(f'No label can be assigned to the main camera.')
            return

    def write_to_registry(self):
        # ----- Write to registry -----
        registry = DeviceRegistry(REGISTRY_PATH)  # uses REGISTRY_PATH by default

        for d in self.devices:
            # only register devices that actually got a label
            label = d.get("label")
            if not label:
                continue

            mac = d.get("mac")
            uuid = d.get("uuid")
            device_type = "camera"
            if self.password is None:
                new_password = secrets.token_urlsafe(8)
            else:
                new_password = self.password

            new = registry.add_registered_device(
                mac_address=mac,
                uuid=uuid,
                device_type=device_type,
                label=label,
                # password= new_password # Password can not yet be saved in the  registry. could be added in the future.
            )

            if new:
                print(f'Device with label: {label} is added to registry.\n')

                # Generate new password (can also change username if neccesary)
                if self.change_password:
                    new_pass = self.onvif.change_user(host=d["host"],
                                                 port=d["port"],
                                                 new_password=new_password)
                    if new_pass:
                        print(f"Changed password for camera {d['host']} to : {new_password}  and saved it to the registry.")
                        # To do: save new password to registry
                    else:
                        print(f"Could not change the password of camera: {d['host']}")
                        # To do: delete new saved password from registry

            else:
                print(f'Device with label: {label} was already known in registry')

        # Save everything to a json file
        registry.persist_to_json()

    def compute_tvec(self,
                     W, H, HFOV_deg, VFOV_deg,
                     quad2, dist=None):
        """"
        Function to compute translation from the camera to an object (QR code)
        """

        # Estimate K matrix using camera specs (measuring K matrix of cam should improve accuracy)
        K = self.make_K(W, H, HFOV_deg, VFOV_deg)

        # Make sure quad2 is of type numpy asarray
        quad2 = np.asarray(quad2, dtype=np.float32)

        # If distortion of the camera is not known(not measured) assume it to have no distortion
        if dist == None:
            dist = np.zeros(5, dtype=np.float32)

        # Create a square to represent the QR code coördinates
        half = self.size_qr / 2
        obj_pts = np.array([
            [-half, half, 0.0],  # top-left
            [half, half, 0.0],  # top-right
            [half, -half, 0.0],  # bottom-right
            [-half, -half, 0.0],  # bottom-left
        ], dtype=np.float32)

        # Turn QR coördinates into ...
        img_pts = quad2.astype(np.float32)  # (4,2)

        ok, rvec, tvec = cv2.solvePnP(
            obj_pts,  # Shape of the QR in 4 coördinates
            img_pts,  # Actual coördinates of the QR from secondary cam
            K,  # Specs from secondary cam in a 3x3 matrix
            dist,  # Distortion of cam2 (assumed to be 0 if not measured)
            flags=cv2.SOLVEPNP_IPPE_SQUARE
        )
        if not ok:
            return None

        return tvec

    def fx_fy_from_fov(self, W, H, HFOV_deg, VFOV_deg):
        """
        Functions to Determine the position of the secondary camera in the FOV of the main camera.
        W = width in # of pixels
        H = heigt in # of pixels
        HFOV = horixontal angle of field of view
        VFOV = vertical angle of field of view
        """
        fx = (W / 2) / math.tan(math.radians(HFOV_deg / 2))
        fy = (H / 2) / math.tan(math.radians(VFOV_deg / 2))
        cx, cy = (W - 1) / 2, (H - 1) / 2
        return fx, fy, cx, cy

    def make_K(self, W, H, HFOV_deg, VFOV_deg):
        """
        Estimate the K matrix of the camera using the camera specs.
        (Measuring the actual K matrix should improve performance)
        """
        fx, fy, cx, cy = self.fx_fy_from_fov(W, H, HFOV_deg, VFOV_deg)
        K = np.array([
            [fx, 0.0, cx],
            [0.0, fy, cy],
            [0.0, 0.0, 1.0]
        ], dtype=np.float32)
        return K

    """
    t_cam:   3-vector from *secondary* cam to MAIN QR (in cam coords)
    main_tvecs: list of 3-vectors from MAIN CAM to each secondary QR (in main coords)

    Assumes:
      - all cams/QRs are on a line opposite the main cam
      - all facing 180° back to the main
      - upright

    then pick the closest main tvec when compared with t_cam.
    """
    def translation_dis(self, t_cam):
        if self.one_cam == False:

            if t_cam is None:
                return None, float("inf")

            t_cam = np.asarray(t_cam, dtype=float).reshape(3, )
            main_arr = np.asarray(self.main_tvecs, dtype=float)

            if main_arr.ndim != 2 or main_arr.shape[1] != 3:
                print("Could not find enough translation vectors in main field of view.")
                raise ValueError("main_tvecs must be list/array of 3-vectors")

            if self.use_y:
                # Use  x and y – horizontal and vertical translation (for Grid like camera setup)
                diff = main_arr[:, [0, 1]] - t_cam[[0, 1]]
                dists = np.linalg.norm(diff, axis=1)
            else:
                # Use only x - horizontal translation (if all cameras are on a line)
                diff = main_arr[:, [0]] - t_cam[[0]]
                dists = np.linalg.norm(diff, axis=1)

            idx = int(np.argmin(dists))
        else:
            main_arr = np.asarray(self.main_tvecs)
            t_cam = np.asarray(t_cam)

            # Ensure main_arr is 2D: (N, 3) even if you pass a single 1D vector
            if main_arr.ndim == 1:
                main_arr = main_arr[None, :]  # shape becomes (1, 3)

            if self.use_y:
                # Use x and y – horizontal and vertical translation
                diff = main_arr[[0, 1]] - t_cam[[0, 1]]
                dists = np.linalg.norm(diff, axis=1)
            else:
                # only x – horizontal translation
                diff = main_arr[[0]] - t_cam[[0]]
                dists = np.linalg.norm(diff, axis=1)

            idx = int(np.argmin(dists))
        return idx, float(dists[idx])


# ----- function to save image and result for debugging and testing -----
def save_result_frame(results, frame, camera):

    for i, r in enumerate(results, start=1):
        print(f"Position {i}; label = {r['label']}; authenticity = {r['authenticity']}.")

    # Draw annotated result
    annotated = frame.copy()
    for i, r in enumerate(results, start=1):
        x, y, w, h = r["bbox"]
        if (r["authenticity"] == True):
            cv2.rectangle(annotated, (x, y), (x + w, y + h), (0, 255, 0), 2)
            cv2.putText(annotated, f"#{i}, {r['label']}, {r['authenticity']}", (x, y - 10),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
        elif (r["label"] == "NOT_DECODED"):
            cv2.rectangle(annotated, (x, y), (x + w, y + h), (255, 0, 0), 2)
            cv2.putText(annotated, f"#{i}, {r['label']}, {r['authenticity']}", (x, y - 10),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 0, 0), 2)
        else:
            cv2.rectangle(annotated, (x, y), (x + w, y + h), (0, 0, 255), 2)
            cv2.putText(annotated, f"#{i}, {r['label']}, {r['authenticity']}", (x, y - 10),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 255), 2)

    # Uncomment the following line to save the scanned image
    cv2.imwrite(f"scanned_{camera}.png", annotated)

    results_file = f"{camera}_results.json"
    data = {
        "device": camera,
        "results": results,
    }
    with open(results_file, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
    return

# det = Cam_register(one_cam=False, save_results=True)
# det.start()
