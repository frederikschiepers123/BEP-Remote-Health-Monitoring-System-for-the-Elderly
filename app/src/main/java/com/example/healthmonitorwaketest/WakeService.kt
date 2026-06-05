package com.example.healthmonitorwaketest

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager

class WakeService : Service() {

    override fun onCreate() {
        super.onCreate()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {

            val channel =
                NotificationChannel(
                    "wake_test",
                    "Wake Test",
                    NotificationManager.IMPORTANCE_LOW
                )

            val manager =
                getSystemService(NotificationManager::class.java)

            manager.createNotificationChannel(channel)
        }

        val notification =
            Notification.Builder(this, "wake_test")
                .setContentTitle("Wake Test Running")
                .build()

        startForeground(1, notification)

        Handler(Looper.getMainLooper()).postDelayed({

            val pm =
                getSystemService(POWER_SERVICE)
                        as PowerManager

            val wakeLock =
                pm.newWakeLock(
                    PowerManager.ACQUIRE_CAUSES_WAKEUP or
                            PowerManager.FULL_WAKE_LOCK,
                    "WakeTest:Wake"
                )

            wakeLock.acquire(5000)

        }, 30000)
    }

    override fun onBind(intent: Intent?): IBinder? = null
}