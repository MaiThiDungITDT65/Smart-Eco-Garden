import type { Metadata } from "next";
import "./globals.css";
export const metadata: Metadata = {
 title: "Smart Eco Garden | Giám sát hệ thống",
 description: "Giám sát và điều khiển vườn sinh thái thông minh"
};
export default function RootLayout({children}:{children:React.ReactNode}) {
 return <html lang="vi"><body>{children}</body></html>;
}
