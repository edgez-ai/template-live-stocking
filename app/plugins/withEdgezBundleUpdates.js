const { withAndroidManifest, withDangerousMod, withMainApplication } = require("expo/config-plugins");
const fs = require("fs");
const path = require("path");

module.exports = function withEdgezBundleUpdates(config, options = {}) {
  const runtimeVersion = options.runtimeVersion;
  if (!runtimeVersion || !/^[A-Za-z0-9_.-]+$/.test(runtimeVersion)) {
    throw new Error("withEdgezBundleUpdates requires a safe runtimeVersion");
  }

  config = withMainApplication(config, (mainApplication) => {
    if (mainApplication.modResults.language !== "kt") {
      throw new Error("EdgeZ bundle updates currently require a Kotlin MainApplication");
    }
    let contents = mainApplication.modResults.contents;
    const importLine = "import ai.edgez.react_native_sdk.EdgezBundleUpdateManager\n";
    if (!contents.includes(importLine.trim())) {
      const anchor = "import android.content.res.Configuration\n";
      if (!contents.includes(anchor)) throw new Error("Cannot add the EdgeZ bundle update import");
      contents = contents.replace(anchor, `${anchor}\n${importLine}`);
    }

    const argument = `      jsBundleFilePath = EdgezBundleUpdateManager.resolveBundleFile(applicationContext, BuildConfig.DEBUG, "${runtimeVersion}"),\n`;
    if (!contents.includes("jsBundleFilePath = EdgezBundleUpdateManager.resolveBundleFile")) {
      const anchor = "      context = applicationContext,\n";
      if (!contents.includes(anchor)) throw new Error("Cannot configure the EdgeZ JS bundle loader");
      contents = contents.replace(anchor, `${anchor}${argument}`);
    }
    mainApplication.modResults.contents = contents;
    return mainApplication;
  });

  config = withAndroidManifest(config, (androidManifest) => {
    const application = androidManifest.modResults.manifest.application?.[0];
    if (!application?.$) throw new Error("Cannot configure the EdgeZ OTA certificate");
    application.$["android:networkSecurityConfig"] = "@xml/edgez_network_security_config";
    return androidManifest;
  });

  config = withDangerousMod(config, ["android", async (androidConfig) => {
    const resources = path.join(androidConfig.modRequest.platformProjectRoot, "app", "src", "main", "res");
    const raw = path.join(resources, "raw");
    const xml = path.join(resources, "xml");
    fs.mkdirSync(raw, { recursive: true });
    fs.mkdirSync(xml, { recursive: true });
    fs.copyFileSync(path.join(__dirname, "..", "certificates", "github-edgez-biz.pem"), path.join(raw, "edgez_ota_ca.pem"));
    fs.writeFileSync(path.join(xml, "edgez_network_security_config.xml"), `<?xml version="1.0" encoding="utf-8"?>
<network-security-config>
  <domain-config cleartextTrafficPermitted="true">
    <domain includeSubdomains="false">localhost</domain>
    <domain includeSubdomains="false">127.0.0.1</domain>
  </domain-config>
  <domain-config>
    <domain includeSubdomains="false">github.edgez.biz</domain>
    <trust-anchors>
      <certificates src="@raw/edgez_ota_ca" />
    </trust-anchors>
  </domain-config>
</network-security-config>
`);
    return androidConfig;
  }]);

  return config;
};
