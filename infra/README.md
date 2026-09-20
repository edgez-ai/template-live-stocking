# Appwrite infrastructure

`npm run deploy` enables email/password auth, disables anonymous auth,
registers web and Android platforms, and installs the row-secured telemetry
table, event-driven MQTT Function, static Next.js Site, variables, and domains.
Devices and MQTT credentials use Appwrite's built-in per-project Devices API.
The Appwrite CLI honors the deployment directory's `.gitignore`; both `site/`
and `function/` exclude local `node_modules/` so dependencies are installed by
the configured remote `npm install` command instead of uploaded.

This changes remote state. Use `npm run check` for local syntax validation.

`npm run clean` removes the resources with this template's deterministic IDs:
the Site and its proxy rule, MQTT Function, telemetry database, and web/local/
Android auth platforms. It does not delete project users, Appwrite Devices, or
change project-wide authentication methods. To inspect the commands without
changing Appwrite, run `INFRA_DRY_RUN=1 npm run clean`.
