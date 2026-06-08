////package com.example.healthmonitorwaketest
////
////import android.content.Intent
////import android.os.Bundle
////import android.widget.Button
////import androidx.appcompat.app.AppCompatActivity
////
////class MainActivity : AppCompatActivity() {
////
////    override fun onCreate(savedInstanceState: Bundle?) {
////        super.onCreate(savedInstanceState)
////
////        val button = Button(this)
////        button.text = "Start Wake Test"
////
////        setContentView(button)
////
////        button.setOnClickListener {
////
////            val intent =
////                Intent(this, WakeService::class.java)
////
////            if (android.os.Build.VERSION.SDK_INT >= 26) {
////                startForegroundService(intent)
////            } else {
////                startService(intent)
////            }
////        }
////    }
////}
//
//package com.example.healthmonitorwaketest
//
//import android.app.admin.DevicePolicyManager
//import android.content.ComponentName
//import android.content.Intent
//import android.os.Build
//import android.os.Bundle
//import android.widget.Button
//import android.widget.LinearLayout
//import androidx.appcompat.app.AppCompatActivity
//import android.content.BroadcastReceiver
//import android.content.IntentFilter
//import android.widget.TextView
//
//class MainActivity : AppCompatActivity() {
//
//    private lateinit var statusText: TextView
//
//    override fun onCreate(savedInstanceState: Bundle?) {
//        super.onCreate(savedInstanceState)
//
//        val layout = LinearLayout(this)
//        layout.orientation = LinearLayout.VERTICAL
//
//        val wakeButton = Button(this)
//        wakeButton.text = "SCREEN ON"
//
//        val offButton = Button(this)
//        offButton.text = "SCREEN OFF"
//
//        statusText = TextView(this)
//        statusText.text = "Waiting for MQTT..."
//
//        layout.addView(wakeButton)
//        layout.addView(offButton)
//        layout.addView(statusText)
//
//        setContentView(layout)
//
//        wakeButton.setOnClickListener {
//
//            val intent = Intent(this, WakeService::class.java)
//
//            if (Build.VERSION.SDK_INT >= 26) {
//                startForegroundService(intent)
//            } else {
//                startService(intent)
//            }
//        }
//
//        offButton.setOnClickListener {
//
//            val dpm =
//                getSystemService(DEVICE_POLICY_SERVICE)
//                        as DevicePolicyManager
//
//            val admin = ComponentName(
//                this,
//                MyDeviceAdminReceiver::class.java
//            )
//
//            if (dpm.isAdminActive(admin)) {
//                dpm.lockNow()
//            }
//        }
//
//        registerReceiver(
//            mqttReceiver,
//            IntentFilter("MQTT_MESSAGE")
//        )
//
//    }
//}

package com.example.healthmonitorwaketest

import android.app.admin.DevicePolicyManager
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.os.Bundle
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity

class MainActivity : AppCompatActivity() {

    private lateinit var statusText: TextView

    private val mqttReceiver =
        object : BroadcastReceiver() {

            override fun onReceive(
                context: Context?,
                intent: Intent?
            ) {

                val message =
                    intent?.getStringExtra("message")
                        ?: "null"

                statusText.text =
                    "Last MQTT message:\n$message"
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val layout = LinearLayout(this)
        layout.orientation = LinearLayout.VERTICAL

        val wakeButton = Button(this)
        wakeButton.text = "SCREEN ON"

        val offButton = Button(this)
        offButton.text = "SCREEN OFF"

        statusText = TextView(this)
        statusText.text = "Waiting for MQTT..."

        layout.addView(statusText)
        layout.addView(wakeButton)
        layout.addView(offButton)

        setContentView(layout)

        wakeButton.setOnClickListener {

            val intent =
                Intent(this, WakeService::class.java)

            if (Build.VERSION.SDK_INT >= 26) {
                startForegroundService(intent)
            } else {
                startService(intent)
            }
        }

        offButton.setOnClickListener {

            val dpm =
                getSystemService(DEVICE_POLICY_SERVICE)
                        as DevicePolicyManager

            val admin = ComponentName(
                this,
                MyDeviceAdminReceiver::class.java
            )

            if (dpm.isAdminActive(admin)) {
                dpm.lockNow()
            }
        }

        registerReceiver(
            mqttReceiver,
            IntentFilter("MQTT_MESSAGE")
        )

        startService(
            Intent(this, MQTTService::class.java)
        )
    }

    override fun onDestroy() {
        super.onDestroy()

        unregisterReceiver(mqttReceiver)
    }
}