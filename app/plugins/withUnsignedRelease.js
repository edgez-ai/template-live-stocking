const { withAppBuildGradle } = require("expo/config-plugins");

module.exports = function withUnsignedRelease(config) {
  return withAppBuildGradle(config, (gradleConfig) => {
    const contents = gradleConfig.modResults.contents;
    const buildTypes = contents.indexOf("    buildTypes {");
    const releaseStart = contents.indexOf("        release {", buildTypes);
    const releaseEnd = contents.indexOf("\n        }", releaseStart);
    const signingLine = "            signingConfig signingConfigs.debug\n";

    if (buildTypes < 0 || releaseStart < 0 || releaseEnd < 0) {
      throw new Error("Cannot locate the Android release build type");
    }

    const releaseBlock = contents.slice(releaseStart, releaseEnd);
    if (!releaseBlock.includes(signingLine)) {
      throw new Error("Cannot remove the generated Android debug release signing config");
    }

    gradleConfig.modResults.contents =
      contents.slice(0, releaseStart) +
      releaseBlock.replace(signingLine, "") +
      contents.slice(releaseEnd);
    return gradleConfig;
  });
};
