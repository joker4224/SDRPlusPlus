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

private val usbReceiver = object : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (ACTION_USB_PERMISSION == intent.action) {
            synchronized(this) {
                var _this = context as MainActivity;
                _this.SDR_device = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
                if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                    _this.SDR_conn = _this.usbManager!!.openDevice(_this.SDR_device);
                    
                    // Save SDR info
                    _this.SDR_VID = _this.SDR_device!!.getVendorId();
                    _this.SDR_PID = _this.SDR_device!!.getProductId()
                    _this.SDR_FD = _this.SDR_conn!!.getFileDescriptor();
                }
                
                // Whatever the hell this does
                context.unregisterReceiver(this);

                // Hide again the system bars
                _this.hideSystemBars();
            }
        }
    }
}

class MainActivity : NativeActivity() {
    private val TAG : String = "SDR++";
    public var usbManager : UsbManager? = null;
    public var SDR_device : UsbDevice? = null;
    public var SDR_conn : UsbDeviceConnection? = null;
    public var SDR_VID : Int = -1;
    public var SDR_PID : Int = -1;
    public var SDR_FD : Int = -1;
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
        val permissionIntent = PendingIntent.getBroadcast(this, 0, Intent(ACTION_USB_PERMISSION), 0)
        val filter = IntentFilter(ACTION_USB_PERMISSION)
        registerReceiver(usbReceiver, filter)

        // Get permission for all USB devices
        val devList = usbManager!!.getDeviceList();
        for ((name, dev) in devList) {
            usbManager!!.requestPermission(dev, permissionIntent);
        }

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
        val flist = aman.list(rsrc);
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
