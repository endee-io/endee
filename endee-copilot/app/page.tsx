import DocumentUpload from "@/components/DocumentUpload";
import ChatWindow from "@/components/ChatWindow";

export default function Home() {
  return (
    <>
      <div className="noise-overlay"></div>
      <div className="ambient-glow"></div>
      
      <main className="app-container">
        <DocumentUpload />
        <ChatWindow />
      </main>
    </>
  );
}
