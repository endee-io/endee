// Configuration
const API_BASE_URL = 'http://localhost:8000';

// DOM Elements
const questionInput = document.getElementById('question-input');
const searchBtn = document.getElementById('search-btn');
const loadingEl = document.getElementById('loading');
const errorEl = document.getElementById('error');
const errorMessageEl = document.getElementById('error-message');
const answerSection = document.getElementById('answer-section');
const answerContent = document.getElementById('answer-content');
const sourcesSection = document.getElementById('sources-section');
const sourcesList = document.getElementById('sources-list');
const initialState = document.getElementById('initial-state');

// Event Listeners
searchBtn.addEventListener('click', handleSearch);
questionInput.addEventListener('keypress', (e) => {
    if (e.key === 'Enter') {
        handleSearch();
    }
});

// Allow clicking example questions
document.querySelectorAll('.example-questions li').forEach(li => {
    li.addEventListener('click', () => {
        questionInput.value = li.textContent;
        handleSearch();
    });
});

async function handleSearch() {
    const question = questionInput.value.trim();
    
    if (!question) return;
    
    // Reset states
    hideAll();
    showLoading();
    searchBtn.disabled = true;
    
    try {
        // Call /ask endpoint
        const response = await fetch(`${API_BASE_URL}/ask`, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                question: question,
                top_k: 5
            })
        });
        
        if (!response.ok) {
            const errorData = await response.json();
            throw new Error(errorData.detail || `HTTP error! status: ${response.status}`);
        }
        
        const data = await response.json();
        
        // Display answer and sources
        displayAnswer(data.answer);
        displaySources(data.sources);
        
        hideLoading();
        
    } catch (error) {
        console.error('Error:', error);
        hideLoading();
        showError(error.message || 'Failed to get answer. Please try again.');
    } finally {
        searchBtn.disabled = false;
        questionInput.focus();
    }
}

function displayAnswer(answer) {
    answerContent.textContent = answer;
    answerSection.classList.remove('hidden');
}

function displaySources(sources) {
    if (!sources || sources.length === 0) {
        return;
    }
    
    sourcesList.innerHTML = '';
    
    sources.forEach((source, index) => {
        const card = document.createElement('div');
        card.className = 'source-card';
        
        // Create metadata tags
        const metaHtml = createMetaTags(source.metadata || {});
        const content = source.text || 'No content available';
        
        card.innerHTML = `
            ${metaHtml}
            <div class="source-content">${escapeHtml(content)}</div>
        `;
        
        sourcesList.appendChild(card);
    });
    
    sourcesSection.classList.remove('hidden');
}

function createMetaTags(metadata) {
    const tags = [];
    
    // Add relevant metadata as tags
    if (metadata.category) tags.push(metadata.category);
    if (metadata.company) tags.push(metadata.company);
    if (metadata.topic) tags.push(metadata.topic);
    if (metadata.difficulty) tags.push(metadata.difficulty);
    
    if (tags.length === 0) {
        return '';
    }
    
    const tagsHtml = tags.map(tag => `<span class="source-tag">${escapeHtml(tag)}</span>`).join('');
    return `<div class="source-meta">${tagsHtml}</div>`;
}

function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
}

function showLoading() {
    loadingEl.classList.remove('hidden');
}

function hideLoading() {
    loadingEl.classList.add('hidden');
}

function showError(message) {
    errorMessageEl.textContent = message;
    errorEl.classList.remove('hidden');
}

function hideAll() {
    answerSection.classList.add('hidden');
    sourcesSection.classList.add('hidden');
    errorEl.classList.add('hidden');
    loadingEl.classList.add('hidden');
    initialState.classList.add('hidden');
}

// Focus input on load
questionInput.focus();
