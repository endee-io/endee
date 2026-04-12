import requests
from bs4 import BeautifulSoup
from urllib.parse import urljoin, urlparse

class DatasetSpider:
    def __init__(self, start_url, max_pages=10):
        self.start_url = start_url
        self.max_pages = max_pages
        self.visited = set()
        self.domain = urlparse(start_url).netloc
        self.chunks = []

    def crawl(self):
        print(f"[Spider] Staring crawl at {self.start_url}")
        queue = [self.start_url]

        while queue and len(self.visited) < self.max_pages:
            url = queue.pop(0)
            if url in self.visited:
                continue

            self.visited.add(url)
            print(f"[Spider] Scraping {url}...")

            try:
                headers = {'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36'}
                resp = requests.get(url, timeout=10, headers=headers)
                if resp.status_code != 200:
                    print(f"[Spider] Skipped {url} (Status: {resp.status_code})")
                    continue

                soup = BeautifulSoup(resp.text, 'html.parser')
                
                # Extract clean text chunks
                paragraphs = soup.find_all(['p', 'h1', 'h2', 'h3', 'li'])
                page_text = "\n".join([p.get_text(strip=True) for p in paragraphs if len(p.get_text(strip=True)) > 20])
                
                # Chunk into windows of ~100 words
                words = page_text.split()
                chunk_size = 100
                for i in range(0, len(words), chunk_size):
                    chunk = " ".join(words[i:i + chunk_size])
                    if len(chunk) > 50:
                        self.chunks.append({"source": url, "text": chunk})

                # Find internal links to continue crawling
                for link in soup.find_all('a', href=True):
                    next_url = urljoin(url, link['href'])
                    if urlparse(next_url).netloc == self.domain and next_url not in self.visited:
                        queue.append(next_url)

            except Exception as e:
                print(f"[Spider] Error scraping {url}: {e}")

        print(f"[Spider] Crawl finished. Extracted {len(self.chunks)} text chunks.")
        return self.chunks

if __name__ == "__main__":
    # Test
    spider = DatasetSpider("https://example.com", max_pages=1)
    results = spider.crawl()
    print(results[:2])
