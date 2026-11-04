package com.totkrecomp;

import android.app.AlertDialog;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

import org.libsdl.app.SDLActivity;

// SDL's activity with the port's library and command line. SDL_main (runtime/android.c) is in libtotk.so, the last
// library listed. The launch intent's "args" extra is the command line, split at spaces, so a probe runs with
//   adb shell am start -n com.totkrecomp/.MainActivity --es args "--probe-kernel"
public class MainActivity extends SDLActivity {
    private static final String TAG = "totk";
    private static final int PICK_ZIP = 7101, PICK_FOLDER = 7102;
    private static final long MAX_BYTES = 1L << 30;   // far above any save: a wrong zip stops here, not on a full disk
    private static final int MAX_DEPTH = 8;

    // Import save (runtime/ui/menu.cpp rows_save): 0 nothing going on, 1 choosing or copying, 2 a copy is in
    // <data folder>/save-import-picked for fs_save_stage_picked, -1 nothing could be copied. Read by the menu's thread.
    private volatile int pickState;

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "totk"};
    }

    @Override
    protected String[] getArguments() {
        String args = getIntent() != null ? getIntent().getStringExtra("args") : null;
        return args == null || args.trim().isEmpty() ? new String[0] : args.trim().split("\\s+");
    }

    // Called on the way out (runtime/android.c android_restart): RestartActivity starts the game again once this process
    // has ended.
    public void restartGame() {
        startActivity(new Intent(this, RestartActivity.class).putExtra("pid", android.os.Process.myPid()));
    }

    // The system's document picker takes a file or a folder, never both on one screen, so the choice comes first.
    public void pickSave(final String title, final String zip, final String folder) {
        pickState = 1;
        runOnUiThread(() -> new AlertDialog.Builder(this)
                .setTitle(title)
                .setItems(new String[] {zip, folder}, (d, which) -> openPicker(which == 0))
                .setOnCancelListener(d -> pickState = 0)
                .show());
    }

    // The state; a finished one (2 or -1) is handed over once.
    public int pickSaveState() {
        int s = pickState;
        if (s == 2 || s == -1) pickState = 0;
        return s;
    }

    private void openPicker(boolean zip) {
        Intent i;
        if (zip) {
            i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            i.putExtra(Intent.EXTRA_MIME_TYPES,
                       new String[] {"application/zip", "application/x-zip-compressed", "application/octet-stream"});
        } else {
            i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        }
        try {
            startActivityForResult(i, zip ? PICK_ZIP : PICK_FOLDER);
        } catch (RuntimeException e) {   // no document picker on this device
            Log.w(TAG, "save import: no file browser: " + e);
            pickState = -1;
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_ZIP && request != PICK_FOLDER) return;
        final Uri uri = data != null ? data.getData() : null;
        if (result != RESULT_OK || uri == null) {
            pickState = 0;
            return;
        }
        final boolean zip = request == PICK_ZIP;
        new Thread(() -> {
            File out = new File(getExternalFilesDir(null), "save-import-picked");
            try {
                deleteTree(out);
                if (!out.mkdirs()) throw new IOException("cannot make " + out);
                long[] left = {MAX_BYTES};
                if (zip) copyZip(uri, out, left);
                else copyTree(uri, DocumentsContract.getTreeDocumentId(uri), out, left, 0);
                pickState = 2;
            } catch (IOException | RuntimeException e) {
                Log.w(TAG, "save import: " + e);
                deleteTree(out);
                pickState = -1;
            }
        }, "save import").start();
    }

    private void copyZip(Uri uri, File out, long[] left) throws IOException {
        try (InputStream raw = getContentResolver().openInputStream(uri);
             ZipInputStream in = new ZipInputStream(raw)) {
            String root = out.getCanonicalPath() + File.separator;
            for (ZipEntry e; (e = in.getNextEntry()) != null; ) {
                File f = new File(out, e.getName().replace('\\', '/'));
                if (!f.getCanonicalPath().startsWith(root)) throw new IOException("a path outside the zip: " + e.getName());
                if (e.isDirectory()) continue;
                File parent = f.getParentFile();
                if (parent != null) parent.mkdirs();
                copyStream(in, f, left);
            }
        }
    }

    private void copyTree(Uri tree, String id, File out, long[] left, int depth) throws IOException {
        if (depth > MAX_DEPTH) return;
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, id);
        String[] cols = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                         DocumentsContract.Document.COLUMN_MIME_TYPE};
        try (Cursor c = getContentResolver().query(children, cols, null, null, null)) {
            if (c == null) throw new IOException("cannot list the folder");
            while (c.moveToNext()) {
                String child = c.getString(0), name = c.getString(1);
                if (name == null || name.contains("/") || name.equals("..") || name.equals(".")) continue;
                File f = new File(out, name);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2))) {
                    if (!f.mkdirs() && !f.isDirectory()) throw new IOException("cannot make " + f);
                    copyTree(tree, child, f, left, depth + 1);
                } else {
                    Uri doc = DocumentsContract.buildDocumentUriUsingTree(tree, child);
                    try (InputStream in = getContentResolver().openInputStream(doc)) {
                        copyStream(in, f, left);
                    }
                }
            }
        }
    }

    private static void copyStream(InputStream in, File f, long[] left) throws IOException {
        byte[] buf = new byte[1 << 16];
        try (OutputStream o = new FileOutputStream(f)) {
            for (int n; (n = in.read(buf)) > 0; ) {
                if ((left[0] -= n) < 0) throw new IOException("more than a save holds");
                o.write(buf, 0, n);
            }
        }
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null)
            for (File k : kids) deleteTree(k);
        f.delete();
    }
}
