import { configureClient } from "./appwrite.mjs";
import { installAuth } from "./auth.mjs";
import { installDatabase } from "./database.mjs";
import { installFunction } from "./function.mjs";
import { installSite } from "./site.mjs";
import { installTimeseries } from "./timeseries.mjs";

configureClient();
// Reconcile the project-scoped Time Series Store first. This keeps it at the
// same declarative layer as TablesDB and prevents unrelated Platform or Domain
// failures from leaving the project without its Store.
await installTimeseries();
installAuth();
installDatabase();
installFunction();
installSite();
