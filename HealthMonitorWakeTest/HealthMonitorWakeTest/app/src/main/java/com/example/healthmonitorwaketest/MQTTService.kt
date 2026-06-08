package com.example.healthmonitorwaketest

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.util.Log
import org.eclipse.paho.client.mqttv3.MqttClient
import org.eclipse.paho.client.mqttv3.MqttConnectOptions

class MQTTService : Service() {

    private var client: MqttClient? = null

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
        startForeground(2, createNotification())

        Thread {
            connectMqtt()
        }.start()
    }

    private fun connectMqtt() {
        try {
            // Use 127.0.0.1 since the broker is running on the same device (Termux)
            val brokerUrl = "tcp://127.0.0.1:1883"
            
            client = MqttClient(
                brokerUrl,
                MqttClient.generateClientId(),
                null
            )

            val options = MqttConnectOptions()
            options.isAutomaticReconnect = true
            options.isCleanSession = true
            options.connectionTimeout = 10

            Log.d("MQTT", "Connecting to $brokerUrl...")
            client?.connect(options)

            Log.d("MQTT", "Connected successfully")
            broadcastStatus("Connected to $brokerUrl")

            client?.subscribe("display") { _, message ->
                val payload = String(message.payload).trim().uppercase()
                Log.d("MQTT", "Received: $payload")
                handleCommand(payload)
            }

        } catch (e: Exception) {
            Log.e("MQTT", "Connection error", e)
            val errorMessage = if (e is org.eclipse.paho.client.mqttv3.MqttException) {
                "MQTT ERROR: ${e.reasonCode} - ${e.message}"
            } else {
                "MQTT ERROR: ${e.message}"
            }
            broadcastStatus(errorMessage)
        }
    }

    private fun handleCommand(payload: String) {
        when (payload) {
            "OFF" -> {
                val dpm = getSystemService(DEVICE_POLICY_SERVICE) as android.app.admin.DevicePolicyManager
                val admin = android.content.ComponentName(this, MyDeviceAdminReceiver::class.java)
                if (dpm.isAdminActive(admin)) {
                    dpm.lockNow()
                    Log.d("MQTT", "Screen OFF triggered")
                } else {
                    Log.e("MQTT", "Device Admin not active")
                    broadcastStatus("Error: Device Admin not active")
                }
            }
            "ON" -> {
                val intent = Intent(this, WakeService::class.java)
                startForegroundService(intent)
                Log.d("MQTT", "Screen ON triggered")
            }
            else -> Log.d("MQTT", "Unknown command: $payload")
        }
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                "mqtt_service",
                "MQTT Service",
                NotificationManager.IMPORTANCE_LOW
            )
            val manager = getSystemService(NotificationManager::class.java)
            manager.createNotificationChannel(channel)
        }
    }

    private fun createNotification(): Notification {
        return Notification.Builder(this, "mqtt_service")
            .setContentTitle("MQTT Service Running")
            .setContentText("Listening for display commands")
            .setSmallIcon(android.R.drawable.ic_dialog_info)
            .build()
    }

    private fun broadcastStatus(message: String) {
        val intent = Intent("MQTT_MESSAGE")
        intent.putExtra("message", message)
        sendBroadcast(intent)
    }

    override fun onDestroy() {
        super.onDestroy()
        try {
            client?.disconnect()
            client?.close()
        } catch (e: Exception) {
            Log.e("MQTT", "Error during disconnect", e)
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null
}
