package io.github.mrchrisster.ftlquest;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * First-run setup (2D panel). The APK ships no game code or data: the player picks their own
 * FTL for iPad 1.6.21 (.ipa, or a zip of FTL.app). We extract the game executable (FTL) and
 * ftl.dat into the app's files folder, refuse App Store-encrypted executables, then start the
 * immersive game. Also opened when an .ipa is shared/opened with this app.
 */
public class SetupActivity extends Activity {
    private static final int REQ_PICK = 1;
    private TextView mStatus;
    private ProgressBar mProgress;
    private Button mPick;
    private final Handler mMain = new Handler(Looper.getMainLooper());

    /** True when FTL and ftl.dat are already in place. */
    public static boolean gameFilesPresent(Activity a) {
        File dir = a.getExternalFilesDir(null);
        if (dir == null) return false;
        File exe = new File(dir, "FTL"), dat = new File(dir, "ftl.dat");
        return exe.isFile() && exe.length() > 1_000_000 && dat.isFile() && dat.length() > 50_000_000;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setPadding(64, 48, 64, 48);
        root.setBackgroundColor(Color.rgb(12, 16, 34));

        TextView title = new TextView(this);
        title.setText("FTL Diorama — setup");
        title.setTextSize(28);
        title.setTextColor(Color.rgb(235, 242, 255));
        root.addView(title);

        mStatus = new TextView(this);
        mStatus.setText("This app doesn't include the game. Pick your own copy of FTL for iPad 1.6.21: "
                + "the .ipa, or a zip of FTL.app. Copies straight from the App Store are encrypted and won't work.");
        mStatus.setTextSize(18);
        mStatus.setTextColor(Color.rgb(170, 200, 240));
        mStatus.setPadding(0, 24, 0, 24);
        root.addView(mStatus);

        mProgress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        mProgress.setMax(100);
        mProgress.setVisibility(View.GONE);
        root.addView(mProgress, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        mPick = new Button(this);
        mPick.setText("Choose FTL .ipa or zip");
        mPick.setTextSize(20);
        mPick.setOnClickListener(v -> pick());
        root.addView(mPick);
        setContentView(root);

        Intent in = getIntent();
        if (in != null && Intent.ACTION_VIEW.equals(in.getAction()) && in.getData() != null) {
            importFrom(in.getData());
        } else if (in != null && Intent.ACTION_SEND.equals(in.getAction())) {
            Uri u = in.getParcelableExtra(Intent.EXTRA_STREAM);
            if (u != null) importFrom(u);
        }
    }

    private void pick() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, REQ_PICK);
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req == REQ_PICK && res == RESULT_OK && data != null && data.getData() != null) importFrom(data.getData());
    }

    private void status(String s) { mMain.post(() -> mStatus.setText(s)); }

    private void importFrom(Uri uri) {
        mPick.setEnabled(false);
        mProgress.setVisibility(View.VISIBLE);
        mProgress.setProgress(0);
        status("Reading your copy of FTL…");
        new Thread(() -> {
            String err = null;
            try {
                err = extract(uri);
            } catch (Exception e) {
                err = "Couldn't read that file: " + e.getMessage();
            }
            final String fe = err;
            mMain.post(() -> {
                mProgress.setVisibility(View.GONE);
                mPick.setEnabled(true);
                if (fe != null) {
                    mStatus.setText(fe);
                } else {
                    mStatus.setText("Done. Starting FTL…");
                    startActivity(new Intent(this, MainActivity.class));
                    finish();
                }
            });
        }, "ftl-import").start();
    }

    /** Returns null on success, or a message for the player. */
    private String extract(Uri uri) throws IOException {
        File dir = getExternalFilesDir(null);
        if (dir == null) return "No storage available.";
        File tmpExe = new File(dir, "FTL.import"), tmpDat = new File(dir, "ftl.dat.import");
        boolean gotExe = false, gotDat = false;
        long total = -1;
        try (android.database.Cursor c = getContentResolver().query(uri, new String[]{"_size"}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) total = c.getLong(0);
        } catch (Exception ignored) {}
        try (InputStream raw = getContentResolver().openInputStream(uri)) {
            if (raw == null) return "Couldn't open that file.";
            CountingStream counted = new CountingStream(raw);
            ZipInputStream zip = new ZipInputStream(counted);
            ZipEntry e;
            byte[] buf = new byte[1 << 16];
            while ((e = zip.getNextEntry()) != null) {
                String name = e.getName();
                // Payload/FTL.app/FTL (.ipa) or FTL.app/FTL (zip of the app bundle)
                boolean isExe = name.matches("(.*/)?[^/]+\\.app/FTL");
                boolean isDat = name.matches("(.*/)?[^/]+\\.app/ftl\\.dat");
                if (!isExe && !isDat) continue;
                File out = isExe ? tmpExe : tmpDat;
                try (OutputStream os = new FileOutputStream(out)) {
                    int n;
                    while ((n = zip.read(buf)) > 0) {
                        os.write(buf, 0, n);
                        if (total > 0) {
                            final int pct = (int) Math.min(99, counted.count * 100 / total);
                            mMain.post(() -> mProgress.setProgress(pct));
                        }
                    }
                }
                if (isExe) gotExe = true; else gotDat = true;
                status(isExe ? "Found the game executable…" : "Found ftl.dat…");
                if (gotExe && gotDat) break;
            }
        }
        if (!gotExe || !gotDat) {
            tmpExe.delete(); tmpDat.delete();
            return "That file doesn't look like FTL for iPad (no FTL.app with FTL and ftl.dat inside). "
                    + "Pick the .ipa or a zip of FTL.app.";
        }
        String enc = checkMachO(tmpExe);
        if (enc != null) { tmpExe.delete(); tmpDat.delete(); return enc; }
        File exe = new File(dir, "FTL"), dat = new File(dir, "ftl.dat");
        exe.delete(); dat.delete();
        if (!tmpExe.renameTo(exe) || !tmpDat.renameTo(dat)) return "Couldn't save the game files.";
        return null;
    }

    /** Rejects App Store encryption (LC_ENCRYPTION_INFO[_64] cryptid != 0) in a thin or fat (universal) Mach-O. */
    private static String checkMachO(File f) throws IOException {
        try (RandomAccessFile r = new RandomAccessFile(f, "r")) {
            int be = r.readInt();
            if (be == 0xCAFEBABE) { // fat: only the arm64 slice runs here (big-endian fat_arch records)
                int n = r.readInt();
                for (int i = 0; i < n && i < 16; i++) {
                    r.seek(8 + 20L * i);
                    int cputype = r.readInt();
                    r.readInt();
                    long off = r.readInt() & 0xFFFFFFFFL;
                    if (cputype == 0x0100000C) return checkSlice(r, off);
                }
                return "This copy has no 64-bit ARM build of FTL. You need FTL for iPad 1.6.21.";
            }
            return checkSlice(r, 0);
        }
    }

    private static String checkSlice(RandomAccessFile r, long base) throws IOException {
        r.seek(base);
        int magic = Integer.reverseBytes(r.readInt());
        if (magic != 0xFEEDFACF && magic != 0xFEEDFACE) return null;
        r.seek(base + 16);
        int ncmds = Integer.reverseBytes(r.readInt());
        long off = base + (magic == 0xFEEDFACF ? 32 : 28);
        for (int i = 0; i < ncmds && i < 512; i++) {
            r.seek(off);
            int cmd = Integer.reverseBytes(r.readInt());
            int size = Integer.reverseBytes(r.readInt());
            if (cmd == 0x2C || cmd == 0x21) { // LC_ENCRYPTION_INFO_64 / LC_ENCRYPTION_INFO
                r.seek(off + 16);
                if (Integer.reverseBytes(r.readInt()) != 0)
                    return "This copy is encrypted (downloaded straight from the App Store), so it can't run here. "
                            + "You need a decrypted copy of your FTL for iPad 1.6.21.";
            }
            if (size <= 0) break;
            off += size;
        }
        return null;
    }

    private static final class CountingStream extends java.io.FilterInputStream {
        long count;
        CountingStream(InputStream in) { super(in); }
        @Override public int read() throws IOException { int b = super.read(); if (b >= 0) count++; return b; }
        @Override public int read(byte[] b, int o, int l) throws IOException { int n = super.read(b, o, l); if (n > 0) count += n; return n; }
        @Override public long skip(long n) throws IOException { long s = super.skip(n); count += s; return s; }
    }
}
