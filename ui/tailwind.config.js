/** @type {import('tailwindcss').Config} */
export default {
  content: [
    "./index.html",
    "./src/**/*.{js,ts,jsx,tsx}",
  ],
  theme: {
    extend: {
      colors: {
        vibe: {
          bg: '#0f172a',
          card: '#1e293b',
          accent: '#3b82f6',
          text: '#f8fafc'
        }
      }
    },
  },
  plugins: [],
}
