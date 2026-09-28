import { configureClient } from "./appwrite.mjs";
import { installAuth } from "./auth.mjs";
import { installDatabase } from "./database.mjs";
import { installFunction } from "./function.mjs";
import { installSite } from "./site.mjs";
import { installTimeseries } from "./timeseries.mjs";

configureClient();
installAuth();
installDatabase();
await installTimeseries();
installFunction();
installSite();
