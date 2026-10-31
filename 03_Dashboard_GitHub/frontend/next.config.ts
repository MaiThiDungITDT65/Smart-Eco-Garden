import type { NextConfig } from "next";
// Same-origin proxy: phone browsers must NEVER call their own localhost:8000.
const config: NextConfig = {
  async rewrites() {
    return [{ source: "/api/eco/:path*", destination: "http://127.0.0.1:8000/api/eco/:path*" }];
  },
};
export default config;
