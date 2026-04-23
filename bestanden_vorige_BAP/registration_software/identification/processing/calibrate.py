import cv2, time
from registration_software.interface.streamers import RTSP_streamer
# from  import QrCodeDetector # returns label, center, bbox, authenticity

def calibrate(device, user, password, detector):
    # open stream to calibrate distance from the camera
    cam = RTSP_streamer(device["host"], device["port"], user, password)
    detect = detector
    cam.start()

    results = None
    try:
        while True:
            frame = cam.read()
            if frame is None:
                # stream might not be ready yet
                time.sleep(0.01)
                continue

            current_frame = frame.copy()

            if results is None and frame is not None:
                frame = cv2.resize(frame, (1000, 1000), interpolation=cv2.INTER_LINEAR)
                cv2.putText(frame, "No QR codes scanned.", (40, 60),
                            cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 0, 0), 2)
                cv2.putText(frame, "Press space to scan and q to quit,",
                            (40, 100), cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 0, 0), 2)
                cv2.imshow("QR codes in FOV", frame)
            elif len(results) == 0 and frame is not None:
                frame = cv2.resize(frame, (1000, 1000), interpolation=cv2.INTER_LINEAR)
                cv2.putText(frame, "No QR codes scanned.", (40, 60),
                            cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 0, 0), 2)
                cv2.putText(frame, "Press space to scan and q to quit.",
                            (40, 100), cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 0, 0), 2)
                # frame = cv2.resize(frame, (1000, 1000), interpolation=cv2.INTER_LINEAR)
                cv2.imshow("QR codes in FOV", frame)
            elif results and frame is not None:
                # frame = cv2.resize(frame, (1000, 1000), interpolation=cv2.INTER_LINEAR)
                for i, r in enumerate(results, start=1):
                    x, y, w, h = r["bbox"]
                    if (r["authenticity"] == True):
                        cv2.rectangle(frame, (x, y), (x + w, y + h), (0, 255, 0), 2)
                        cv2.putText(frame, f"#{i}, {r['label']}, {r['authenticity']}", (x, y - 10),
                                    cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
                    elif (r["label"] == "NOT_DECODED"):
                        cv2.rectangle(frame, (x, y), (x + w, y + h), (255, 0, 0), 2)
                        cv2.putText(frame, f"#{i}, {r['label']}, {r['authenticity']}", (x, y - 10),
                                    cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 0, 0), 2)
                    else:
                        cv2.rectangle(frame, (x, y), (x + w, y + h), (0, 0, 255), 2)
                        cv2.putText(frame, f"#{i}, {r['label']}, {r['authenticity']}", (x, y - 10),
                                    cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 255), 2)


                frame = cv2.resize(frame, (1000, 1000), interpolation=cv2.INTER_LINEAR)
                cv2.putText(frame, f"{len(results)} QR codes scanned.",
                            (40, 60), cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 0, 0), 2)
                cv2.putText(frame, "Press space to scan and q to quit.",
                            (40, 100), cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 0, 0), 2)

                cv2.imshow("QR codes in FOV", frame,)


            key = cv2.waitKey(1)

            if key == ord(" "):
                # frame = get_frame(device["snapshot_url"], user=user, password=password) # Should not be neccesary
                results = detect.detect_single_frame(current_frame)
            elif key == ord('q'):
                break
            # time.sleep(0.01)
    except KeyboardInterrupt:
        pass
    finally:
        cam.stop()
        cv2.destroyAllWindows()