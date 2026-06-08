package com.example.healthmonitorwaketest

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.util.Log

class WakeService : Service() {

    override fun onCreate() {
        super.onCreate()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                "wake_test",
                "Wake Test",
                NotificationManager.IMPORTANCE_LOW
            )
            val manager = getSystemService(NotificationManager::class.java)
            manager.createNotificationChannel(channel)
        }

        val notification = Notification.Builder(this, "wake_test")
            .setContentTitle("Wake Service Active")
            .setSmallIcon(android.R.drawable.ic_dialog_info)
            .build()

        startForeground(1, notification)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        Log.d("WakeService", "onStartCommand triggered - Performing Wake")
        performWake()
        return START_STICKY
    }

    private fun performWake() {
        val pm = getSystemService(POWER_SERVICE) as PowerManager
        val wakeLock = pm.newWakeLock(
            PowerManager.ACQUIRE_CAUSES_WAKEUP or
                    PowerManager.FULL_WAKE_LOCK,
            "WakeTest:Wake"
        )
        
        // Acquire wake lock for 5 seconds to turn the screen on
        wakeLock.acquire(5000)
        Log.d("WakeService", "WakeLock acquired")
    }

    override fun onBind(intent: Intent?): IBinder? = null
}
