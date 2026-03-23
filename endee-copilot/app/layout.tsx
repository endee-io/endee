import "./globals.css";

export const metadata = {
  title: "EndeeCopilot - Premium RAG",
  description: "An intelligent Copilot powered by Endee Vector DB",
};

export default function RootLayout({
  children,
}: {
  children: React.ReactNode;
}) {
  return (
    <html lang="en">
      <body>{children}</body>
    </html>
  );
}
