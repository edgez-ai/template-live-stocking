# Appwrite infrastructure

`npm run deploy` enables email/password auth, disables anonymous auth,
allows farm team members to see each other's user ID, email, and name,
registers web and Android platforms, and installs the row-secured farms,
telemetry, and current-state topology tables, event-driven MQTT Function, static Next.js Site, variables,
and domains. Authenticated users create farm teams and rows from the mobile
Settings screen; each farm team controls access to its devices and telemetry.
Devices and MQTT credentials use Appwrite's built-in per-project Devices API.
The Appwrite CLI honors the deployment directory's `.gitignore`; both `site/`
and `function/` exclude local `node_modules/` so dependencies are installed by
the configured remote `npm install` command instead of uploaded.

This changes remote state. Use `npm run check` for local syntax validation.

`npm run clean` removes the resources with this template's deterministic IDs:
the Site and its proxy rule, MQTT Function, database with farms, telemetry, and topology links,
and web/local/Android auth platforms. It does not delete farm teams, project users, Appwrite Devices, or
change project-wide authentication methods. To inspect the commands without
changing Appwrite, run `INFRA_DRY_RUN=1 npm run clean`.
