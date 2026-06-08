//package com.example.healthmonitorwaketest
//
//import android.app.Service
//import android.content.Intent
//import android.os.IBinder
//import android.util.Log
//import org.eclipse.paho.client.mqttv3.*
//
//class MQTTService : Service() {
//
//    private lateinit var client: MqttClient
//
//    override fun onCreate() {
//        super.onCreate()
//
//        try {
//
//            client = MqttClient(
//                "tcp://127.0.0.1:1883",
//                MqttClient.generateClientId(),
//                null
//            )
//
//            client.connect()
//
//            Log.d("MQTT", "Connected successfully")
//
//            client.subscribe("display") { _, message ->
//
//                val payload = String(message.payload)
//
//                if (payload == "OFF") {
//
//                    val dpm =
//                        getSystemService(DEVICE_POLICY_SERVICE)
//                                as android.app.admin.DevicePolicyManager
//
//                    val admin =
//                        android.content.ComponentName(
//                            this,
//                            MyDeviceAdminReceiver::class.java
//                        )
//
//                    if (dpm.isAdminActive(admin)) {
//                        dpm.lockNow()
//                    }
//                }
//            }
//
//            Log.d("MQTT", "Subscribed successfully")
//
//        } catch (e: Exception) {
//            Log.e("MQTT", "Error", e)
//        }
//    }
//
//    override fun onBind(intent: Intent?): IBinder? = null
//}

package com.example.healthmonitorwaketest

import android.app.Service
import android.content.Intent
import android.os.IBinder
import android.util.Log
import org.eclipse.paho.client.mqttv3.MqttClient

class MQTTService : Service() {

    private lateinit var client: MqttClient

    override fun onCreate() {
        super.onCreate()

        try {

            client = MqttClient(
                "tcp://127.0.0.1:1883",
                MqttClient.generateClientId(),
                null
            )

            client.connect()

            Log.d(
                "MQTT",
                "Connected successfully"
            )

            client.subscribe("display") { _, message ->

                val payload = String(message.payload).trim().uppercase()

                Log.d("MQTT", "Received: $payload")

                when (payload) {

                    "OFF" -> {

                        val dpm =
                            getSystemService(DEVICE_POLICY_SERVICE)
                                    as android.app.admin.DevicePolicyManager

                        val admin =
                            android.content.ComponentName(
                                this,
                                MyDeviceAdminReceiver::class.java
                            )

                        if (dpm.isAdminActive(admin)) {
                            dpm.lockNow()
                        }

                        Log.d("MQTT", "Screen OFF triggered")
                    }

                    "ON" -> {

                        val intent =
                            Intent(this, WakeService::class.java)

                        startForegroundService(intent)

                        Log.d("MQTT", "Screen ON triggered")
                    }

                    else -> {
                        Log.d("MQTT", "Unknown command: $payload")
                    }
                }
            }

            Log.d(
                "MQTT",
                "Subscribed successfully"
            )

        } catch (e: Exception) {

            Log.e(
                "MQTT",
                "Error",
                e
            )

            val intent =
                Intent("MQTT_MESSAGE")

            intent.putExtra(
                "message",
                "MQTT ERROR: ${e.message}"
            )

            sendBroadcast(intent)
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null
}