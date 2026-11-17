package dev.tessera.internal;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Locale;

/**
 * Finds and loads the native library.
 *
 * <ol>
 *   <li>{@code -Dtessera.library.path=<file>} wins (development, tests, system installs).
 *   <li>Otherwise the copy embedded in this jar ({@code /natives/<os>-<arch>/...}) is written to a fresh, private temp
 *       directory, checked against the checksum of the embedded bytes, and loaded by absolute path. A fresh directory per
 *       run means nobody can plant a file where we will load from, and an old copy can never be loaded by mistake.
 * </ol>
 *
 * <p>JNI allows a library to be loaded by only one classloader. If a second classloader in the same JVM (say two plugins
 * each bundling this jar) asks, it gets its own private copy: independent native state, no clash. The FFM binding has no
 * such restriction and shares one instance.
 */
public final class NativeLoader {

    private NativeLoader() {
    }

    public static String osName() {
        final String os = System.getProperty("os.name", "").toLowerCase(Locale.ROOT);
        if (os.contains("win")) {
            return "windows";
        }
        if (os.contains("mac") || os.contains("darwin")) {
            return "macos";
        }
        return "linux";
    }

    public static String archName() {
        final String arch = System.getProperty("os.arch", "").toLowerCase(Locale.ROOT);
        if (arch.equals("aarch64") || arch.equals("arm64")) {
            return "aarch64";
        }
        if (arch.equals("amd64") || arch.equals("x86_64") || arch.equals("x64")) {
            return "x86_64";
        }
        return arch;
    }

    static String libraryFileName() {
        switch (osName()) {
            case "windows":
                return "tessera.dll";
            case "macos":
                return "libtessera.dylib";
            default:
                return "libtessera.so";
        }
    }

    /** The native library file this classloader should use (never loads it). */
    public static Path locate() throws IOException {
        final String property = System.getProperty("tessera.library.path");
        if (property != null && !property.isEmpty()) {
            final Path path = Paths.get(property).toAbsolutePath();
            if (!Files.isRegularFile(path)) {
                throw new FileNotFoundException("tessera.library.path does not name a file: " + path);
            }
            return path;
        }
        final String resource = "/natives/" + osName() + "-" + archName() + "/" + libraryFileName();
        try (InputStream in = NativeLoader.class.getResourceAsStream(resource)) {
            if (in == null) {
                throw new FileNotFoundException("no embedded Tessera library for " + osName() + "-" + archName() + " (" + resource
                        + "); set -Dtessera.library.path=<file>");
            }
            final byte[] bytes = in.readAllBytes();
            final String expected = sha256(bytes);
            final Path directory = Files.createTempDirectory("tessera-" + expected.substring(0, 12) + "-"); // private (0700 on POSIX)
            final Path file = directory.resolve(libraryFileName());
            Files.write(file, bytes, StandardOpenOption.CREATE_NEW, StandardOpenOption.WRITE);
            if (!sha256(Files.readAllBytes(file)).equals(expected)) {
                throw new IOException("extracted Tessera library does not match the embedded checksum: " + file);
            }
            deleteOnExit(file, directory);
            return file;
        }
    }

    /** System.load with the one-classloader rule handled (see class comment). */
    static void loadJni(Path library) {
        try {
            System.load(library.toString());
        } catch (UnsatisfiedLinkError e) {
            final String message = String.valueOf(e.getMessage());
            if (!message.contains("already loaded in another classloader")) {
                throw e;
            }
            try {
                final Path directory = Files.createTempDirectory("tessera-copy-");
                final Path copy = directory.resolve(libraryFileName());
                Files.copy(library, copy, StandardCopyOption.REPLACE_EXISTING);
                deleteOnExit(copy, directory);
                System.load(copy.toString());
            } catch (IOException io) {
                final UnsatisfiedLinkError error = new UnsatisfiedLinkError("cannot make a private copy of the Tessera library: " + io);
                error.initCause(io);
                throw error;
            }
        }
    }

    private static void deleteOnExit(Path file, Path directory) {
        // Best effort: Windows keeps a loaded DLL locked, in which case the OS temp cleanup gets it eventually.
        final File f = file.toFile();
        final File d = directory.toFile();
        d.deleteOnExit(); // registered first => deleted last
        f.deleteOnExit();
    }

    private static String sha256(byte[] data) {
        try {
            final byte[] digest = MessageDigest.getInstance("SHA-256").digest(data);
            final StringBuilder sb = new StringBuilder(digest.length * 2);
            for (byte b : digest) {
                sb.append(String.format("%02x", b));
            }
            return sb.toString();
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException("SHA-256 is required by the Java platform", e);
        }
    }
}
