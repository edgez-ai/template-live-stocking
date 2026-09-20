import type { Metadata } from "next";
import "./styles.css";

export const metadata: Metadata = {
  title: "IoT Provisioning",
  description: "Authenticated device and MQTT telemetry dashboard",
};

export default function RootLayout({ children }: Readonly<{ children: React.ReactNode }>) {
  return <html lang="en"><body>{children}</body></html>;
}
