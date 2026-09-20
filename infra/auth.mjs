import { androidApplicationId, config, ensure, run, webDomain } from "./appwrite.mjs";

export function installAuth() {
  run(["project", "update-auth-method", "--method-id", "email-password", "--enabled", "true"]);
  run(["project", "update-auth-method", "--method-id", "jwt", "--enabled", "true"]);
  run(["project", "update-auth-method", "--method-id", "anonymous", "--enabled", "false"]);

  ensure(
    "web auth platform",
    ["project", "get-platform", "--platform-id", `${config.name}-web`],
    ["project", "create-web-platform", "--platform-id", `${config.name}-web`, "--name", `${config.name} web`, "--hostname", webDomain],
  );
  ensure(
    "local web auth platform",
    ["project", "get-platform", "--platform-id", `${config.name}-local`],
    ["project", "create-web-platform", "--platform-id", `${config.name}-local`, "--name", `${config.name} local web`, "--hostname", "localhost"],
  );
  ensure(
    "Android auth platform",
    ["project", "get-platform", "--platform-id", `${config.name}-android`],
    ["project", "create-android-platform", "--platform-id", `${config.name}-android`, "--name", `${config.name} Android`, "--application-id", androidApplicationId],
  );
}
