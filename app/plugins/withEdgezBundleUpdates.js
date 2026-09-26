const { withMainApplication } = require("expo/config-plugins");

module.exports = function withEdgezBundleUpdates(config, options = {}) {
  const runtimeVersion = options.runtimeVersion;
  if (!runtimeVersion || !/^[A-Za-z0-9_.-]+$/.test(runtimeVersion)) {
    throw new Error("withEdgezBundleUpdates requires a safe runtimeVersion");
  }

  return withMainApplication(config, (mainApplication) => {
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
};
