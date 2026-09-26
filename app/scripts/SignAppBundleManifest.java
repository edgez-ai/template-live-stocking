import java.nio.file.Files;
import java.nio.file.Path;
import java.security.KeyStore;
import java.security.PrivateKey;
import java.security.Signature;

public final class SignAppBundleManifest {
    public static void main(String[] args) throws Exception {
        if (args.length != 4) {
            throw new IllegalArgumentException("Usage: SignAppBundleManifest <keystore> <alias> <payload> <signature>");
        }
        String passwordValue = System.getenv("ANDROID_KEYSTORE_PASSWORD");
        if (passwordValue == null || passwordValue.isEmpty()) throw new IllegalArgumentException("Missing ANDROID_KEYSTORE_PASSWORD");
        char[] password = passwordValue.toCharArray();
        KeyStore keyStore = KeyStore.getInstance("PKCS12");
        try (var input = Files.newInputStream(Path.of(args[0]))) {
            keyStore.load(input, password);
        }
        PrivateKey key = (PrivateKey) keyStore.getKey(args[1], password);
        if (key == null) throw new IllegalArgumentException("Android signing key alias was not found");
        String algorithm = switch (key.getAlgorithm().toUpperCase()) {
            case "RSA" -> "SHA256withRSA";
            case "EC", "ECDSA" -> "SHA256withECDSA";
            default -> throw new IllegalArgumentException("Unsupported Android signing key algorithm: " + key.getAlgorithm());
        };
        Signature signer = Signature.getInstance(algorithm);
        signer.initSign(key);
        signer.update(Files.readAllBytes(Path.of(args[2])));
        Files.write(Path.of(args[3]), signer.sign());
    }
}
