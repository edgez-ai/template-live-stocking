import { configureClient } from "./appwrite.mjs";
import { installAuth } from "./auth.mjs";
import { installDatabase } from "./database.mjs";
import { installFunction } from "./function.mjs";
import { installSite } from "./site.mjs";

configureClient();
installAuth();
installDatabase();
installFunction();
installSite();
