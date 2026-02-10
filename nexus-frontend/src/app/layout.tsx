import type { Metadata } from 'next'
import { Inter } from 'next/font/google'
import '@/styles/globals.css'
import 'reactflow/dist/style.css'

const inter = Inter({ subsets: ['latin'] })

export const metadata: Metadata = {
  title: 'Nexus - AI Knowledge Network',
  description: 'Transform static knowledge into a living intelligence graph',
  keywords: ['AI', 'Knowledge Graph', 'Vector Database', 'Semantic Search', 'Machine Learning'],
  authors: [{ name: 'Nexus Team' }],
  icons: {
    icon: '/favicon.ico',
  },
  openGraph: {
    title: 'Nexus - Self-Evolving AI Knowledge Network',
    description: 'Vector-native knowledge intelligence system',
    type: 'website',
  }
}

export default function RootLayout({
  children,
}: {
  children: React.ReactNode
}) {
  return (
    <html lang="en">
      <body className={inter.className}>
        {children}
      </body>
    </html>
  )
}
