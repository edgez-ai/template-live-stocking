const {
  withAppBuildGradle,
  withProjectBuildGradle,
} = require("expo/config-plugins");

const ORGANIC_MAPS_REPOSITORY = `
    ivy {
      url 'https://github.com/edgez-ai/organicmaps/releases/download/v0.0.5'
      patternLayout { artifact '[artifact]-[revision].[ext]' }
      metadataSources { artifact() }
      content { includeGroup 'ai.edgez.organicmaps' }
    }`;

module.exports = function withOrganicMaps(config) {
  config = withProjectBuildGradle(config, (gradleConfig) => {
    let contents = gradleConfig.modResults.contents;
    if (!contents.includes("edgez-ai/organicmaps/releases/download/v0.0.5")) {
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
    gradleConfig.modResults.contents = contents;
    return gradleConfig;
  });

  return config;
};
