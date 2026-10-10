package org.sdrpp.sdrpp;

import android.app.NativeActivity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.DialogInterface;
import android.content.pm.PackageManager;
import android.hardware.usb.*;
import android.Manifest;
import android.os.Bundle;
import android.os.Build;
import android.os.ParcelFileDescriptor;
import android.view.View;
import android.view.ViewGroup;
import android.view.KeyEvent;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputConnectionWrapper;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.graphics.Color;
import android.util.Log;
import android.content.res.AssetManager;

import androidx.core.app.ActivityCompat;

import androidx.core.content.PermissionChecker;

import java.util.concurrent.LinkedBlockingQueue;
import java.io.*;

private const val ACTION_USB_PERMISSION = "org.sdrpp.sdrpp.USB_PERMISSION";

class MainActivity : NativeActivity() {
    private val TAG : String = "SDR++";
    public var usbManager : UsbManager? = null;
    private lateinit var devices: DeviceManager<UsbDevice, UsbDeviceConnection>
    private var usbReceiverRegistered = false
    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val device = intent.getParcelableExtra<UsbDevice>(UsbManager.EXTRA_DEVICE) ?: return
            when (intent.action) {
                ACTION_USB_PERMISSION -> devices.permissionResult(device,
                    intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false))
                UsbManager.ACTION_USB_DEVICE_DETACHED -> devices.detached(device)
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> devices.refresh()
            }
            hideSystemBars()
        }
    }
    private lateinit var imeInput: EditText;

    // NativeActivity only forwards key events. An IME sends composed words through
    // InputConnection instead, so give it an editor and forward committed text.
    private inner class ImeInput(context: Context) : EditText(context) {
        override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection? {
            val connection = super.onCreateInputConnection(outAttrs) ?: return null
            return object : InputConnectionWrapper(connection, true) {
                override fun commitText(text: CharSequence, newCursorPosition: Int): Boolean {
                    val result = super.commitText(text, newCursorPosition)
                    if (result) queueText(text)
                    return result
                }

                override fun finishComposingText(): Boolean {
                    val editable = this@ImeInput.text
                    val start = BaseInputConnection.getComposingSpanStart(editable)
                    val end = BaseInputConnection.getComposingSpanEnd(editable)
                    val committed = if (start >= 0 && end > start) editable.subSequence(start, end).toString() else ""
                    val result = super.finishComposingText()
                    if (result) queueText(committed)
                    return result
                }

                override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                    val composing = BaseInputConnection.getComposingSpanStart(this@ImeInput.text) >= 0
                    val result = super.deleteSurroundingText(beforeLength, afterLength)
                    if (result && !composing) {
                        repeat(beforeLength.coerceAtMost(1024)) { unicodeCharacterQueue.offer(-1) }
                        repeat(afterLength.coerceAtMost(1024)) { unicodeCharacterQueue.offer(-2) }
                    }
                    return result
                }
            }
        }
    }

    private fun queueText(text: CharSequence) {
        text.toString().codePoints().forEach { unicodeCharacterQueue.offer(it) }
    }

    fun checkAndAsk(permission: String) {
        if (PermissionChecker.checkSelfPermission(this, permission) != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(this, arrayOf(permission), 1);
        }
    }

    public fun hideSystemBars() {
        val decorView = getWindow().getDecorView();
        val uiOptions = View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY;
        decorView.setSystemUiVisibility(uiOptions);
    }

    public override fun onCreate(savedInstanceState: Bundle?) {
        // Hide bars
        hideSystemBars();

        // Ask for required permissions, without these the app cannot run.
        checkAndAsk(Manifest.permission.WRITE_EXTERNAL_STORAGE);
        checkAndAsk(Manifest.permission.READ_EXTERNAL_STORAGE);

        // TODO: Have the main code wait until these two permissions are available

        // Register events
        usbManager = getSystemService(Context.USB_SERVICE) as UsbManager;
        // UsbManager fills broadcast extras: Android 12+ needs FLAG_MUTABLE.
        // Compile SDK 28 has no named constant, so use its documented value.
        val flags = PendingIntent.FLAG_UPDATE_CURRENT or
            (if (Build.VERSION.SDK_INT >= 31) 0x02000000 else 0)
        val permissionIntent = PendingIntent.getBroadcast(this, 0,
            Intent(ACTION_USB_PERMISSION).setPackage(packageName), flags)
        devices = DeviceManager(
            devices = { usbManager!!.deviceList.values.toList() },
            key = { it.deviceName },
            hasPermission = { usbManager!!.hasPermission(it) },
            requestPermission = { device -> runOnUiThread {
                try { usbManager!!.requestPermission(device, permissionIntent) }
                catch (e: Exception) { devices.permissionResult(device, false); Log.e(TAG, "USB permission request failed", e) }
            } },
            open = { device ->
                try { usbManager!!.openDevice(device) }
                catch (e: Exception) { Log.e(TAG, "USB openDevice failed for ${device.deviceName}", e); null }
            },
            close = { it.close() },
            valid = { it.fileDescriptor >= 0 }
        )
        val filter = IntentFilter(ACTION_USB_PERMISSION)
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
        registerReceiver(usbReceiver, filter)
        usbReceiverRegistered = true

        // Ask for internet permission
        checkAndAsk(Manifest.permission.INTERNET);

        super.onCreate(savedInstanceState)

        // Keep the native surface visible while the IME has a real text editor to focus.
        imeInput = ImeInput(this).apply {
            setSingleLine(true)
            imeOptions = EditorInfo.IME_ACTION_DONE or EditorInfo.IME_FLAG_NO_EXTRACT_UI
            setBackgroundColor(Color.TRANSPARENT)
            setTextColor(Color.TRANSPARENT)
            setCursorVisible(false)
            alpha = 0f
            setShowSoftInputOnFocus(false)
            setOnEditorActionListener { _, actionId, _ ->
                if (actionId == EditorInfo.IME_ACTION_DONE) {
                    unicodeCharacterQueue.offer(-3)
                    true
                } else false
            }
        }
        (window.decorView as ViewGroup).addView(imeInput, FrameLayout.LayoutParams(1, 1))
    }

    public override fun onResume() {
        // Hide bars again
        hideSystemBars();
        super.onResume();
    }

    override fun onDestroy() {
        super.onDestroy()
        if (usbReceiverRegistered) {
            unregisterReceiver(usbReceiver)
            usbReceiverRegistered = false
        }
        devices.closeAll()
    }

    // JNI obtains one atomic snapshot. RTL-SDR requests an owned duplicate FD.
    fun getUsbDeviceInfo(allowed: IntArray, duplicate: Boolean): IntArray {
        return try {
            devices.withDevice(
                matches = { device -> allowed.isEmpty() || allowed.indices.step(2).any {
                    it + 1 < allowed.size && allowed[it] == device.vendorId && allowed[it + 1] == device.productId
                } },
                use = { device, connection ->
                    val fd = if (duplicate) ParcelFileDescriptor.fromFd(connection.fileDescriptor).detachFd()
                        else connection.fileDescriptor
                    intArrayOf(fd, device.vendorId, device.productId)
                }
            ) ?: intArrayOf(-1, -1, -1)
        } catch (e: Exception) {
            Log.e(TAG, "USB descriptor acquisition failed", e)
            intArrayOf(-1, -1, -1)
        }
    }

    fun refreshUsbDevices() { devices.refresh() }
    fun getUsbStatus(): String = devices.getStatus()

    fun showSoftInput() {
        runOnUiThread {
            imeInput.text.clear()
            imeInput.requestFocus()
            val inputMethodManager = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
            inputMethodManager.showSoftInput(imeInput, InputMethodManager.SHOW_IMPLICIT)
        }
    }

    fun hideSoftInput() {
        runOnUiThread {
            val inputMethodManager = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
            inputMethodManager.hideSoftInputFromWindow(imeInput.windowToken, 0)
            imeInput.clearFocus()
            hideSystemBars()
        }
    }

    // Queue for the Unicode characters to be polled from native code (via pollUnicodeChar())
    private var unicodeCharacterQueue: LinkedBlockingQueue<Int> = LinkedBlockingQueue()

    // We assume dispatchKeyEvent() of the NativeActivity is actually called for every
    // KeyEvent and not consumed by any View before it reaches here
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.action == KeyEvent.ACTION_DOWN) {
            if (::imeInput.isInitialized && imeInput.hasFocus()) {
                when (event.keyCode) {
                    KeyEvent.KEYCODE_DEL -> unicodeCharacterQueue.offer(-1)
                    KeyEvent.KEYCODE_FORWARD_DEL -> unicodeCharacterQueue.offer(-2)
                    KeyEvent.KEYCODE_ENTER -> unicodeCharacterQueue.offer(-3)
                    KeyEvent.KEYCODE_DPAD_LEFT -> unicodeCharacterQueue.offer(-4)
                    KeyEvent.KEYCODE_DPAD_RIGHT -> unicodeCharacterQueue.offer(-5)
                }
            }
            val unicode = event.getUnicodeChar(event.metaState)
            if (unicode > 0 && unicode != '\n'.toInt() && unicode != '\b'.toInt()) {
                unicodeCharacterQueue.offer(unicode)
            }
        }
        return super.dispatchKeyEvent(event)
    }

    fun pollUnicodeChar(): Int {
        return unicodeCharacterQueue.poll() ?: 0
    }

    public fun createIfDoesntExist(path: String) {
        // This is a directory, create it in the filesystem
        var folder = File(path);
        var success = true;
        if (!folder.exists()) {
            success = folder.mkdirs();
        }
        if (!success) {
            Log.e(TAG, "Could not create folder with path " + path);
        }
    }

    public fun extractDir(aman: AssetManager, local: String, rsrc: String): Int {
        val flist = aman.list(rsrc) ?: emptyArray();
        var ecount = 0;
        for (fp in flist) {
            val lpath = local + "/" + fp;
            val rpath = rsrc + "/" + fp;

            Log.w(TAG, "Extracting '" + rpath + "' to '" + lpath + "'");

            // Create local path if non-existent
            createIfDoesntExist(local);
            
            // Create if directory
            val ext = extractDir(aman, lpath, rpath);

            // Extract if file
            if (ext == 0) {
                // This is a file, extract it
                val _os = FileOutputStream(lpath);
                val _is = aman.open(rpath);
                val ilen = _is.available();
                var fbuf = ByteArray(ilen);
                _is.read(fbuf, 0, ilen);
                _os.write(fbuf);
                _os.close();
                _is.close();
            }

            ecount++;
        }
        return ecount;
    }

    public fun getAppDir(): String {
        val fdir = getFilesDir().getAbsolutePath();

        // Extract all resources to the app directory
        val aman = getAssets();
        extractDir(aman, fdir + "/res", "res");
        createIfDoesntExist(fdir + "/modules");

        return fdir;
    }
}
