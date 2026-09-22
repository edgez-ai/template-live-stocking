const {
  withAppBuildGradle,
  withProjectBuildGradle,
} = require("expo/config-plugins");
const fs = require("fs");
const path = require("path");

const sdkRoot = path.dirname(require.resolve("@edgez/react-native-sdk/package.json"));
const sdkGradle = fs.readFileSync(path.join(sdkRoot, "android/build.gradle"), "utf8");
const repositoryUrl = sdkGradle.match(/https:\/\/github\.com\/edgez-ai\/organicmaps\/releases\/download\/v[\d.]+/)?.[0];
if (!repositoryUrl) throw new Error("Cannot determine the Organic Maps release used by the SDK");

const ORGANIC_MAPS_REPOSITORY = `
    ivy {
      url '${repositoryUrl}'
      patternLayout { artifact '[artifact]-[revision].[ext]' }
      metadataSources { artifact() }
      content { includeGroup 'ai.edgez.organicmaps' }
    }`;

module.exports = function withOrganicMaps(config) {
  config = withProjectBuildGradle(config, (gradleConfig) => {
    let contents = gradleConfig.modResults.contents;
    contents = contents.replace(/https:\/\/github\.com\/edgez-ai\/organicmaps\/releases\/download\/v[\d.]+/g, repositoryUrl);
    if (!contents.includes(repositoryUrl)) {
      const anchor = "    maven { url 'https://www.jitpack.io' }";
      if (!contents.includes(anchor)) throw new Error("Cannot add the Organic Maps Ivy repository");
      contents = contents.replace(anchor, `${anchor}${ORGANIC_MAPS_REPOSITORY}`);
    }
    gradleConfig.modResults.contents = contents;
    return gradleConfig;
  });

  config = withAppBuildGradle(config, (gradleConfig) => {
    let contents = gradleConfig.modResults.contents;
    contents = contents.replace(
      "minSdkVersion rootProject.ext.minSdkVersion",
      "minSdkVersion 26",
    );
    if (!contents.includes("coreLibraryDesugaringEnabled true")) {
      const anchor = "    defaultConfig {";
      if (!contents.includes(anchor)) throw new Error("Cannot enable Organic Maps desugaring");
      contents = contents.replace(anchor, `    compileOptions {
        coreLibraryDesugaringEnabled true
        sourceCompatibility JavaVersion.VERSION_17
        targetCompatibility JavaVersion.VERSION_17
    }
${anchor}`);
    }
    if (!contents.includes("com.android.tools:desugar_jdk_libs")) {
      const anchor = "dependencies {";
      contents = contents.replace(anchor, `${anchor}
    coreLibraryDesugaring("com.android.tools:desugar_jdk_libs:2.1.5")`);
    }
    // The map SDK selects OkHttp 5.3.2, so React Native's cookie adapter must match.
    if (!contents.includes("com.squareup.okhttp3:okhttp-urlconnection:5.3.2")) {
      const anchor = "dependencies {";
      contents = contents.replace(anchor, `${anchor}
    implementation("com.squareup.okhttp3:okhttp-urlconnection:5.3.2")`);
    }
    gradleConfig.modResults.contents = contents;
    return gradleConfig;
  });

  return config;
};
