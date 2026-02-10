"""
Document Processor
Handles document upload, text extraction, and chunking
"""

import logging
from pathlib import Path
from typing import List, Dict, Any
import uuid
from datetime import datetime
import PyPDF2
from docx import Document as DocxDocument
import asyncio

logger = logging.getLogger(__name__)

class DocumentChunk:
    """Represents a chunk of text from a document"""
    
    def __init__(
        self,
        chunk_id: str,
        document_id: str,
        text: str,
        chunk_index: int,
        metadata: Dict[str, Any]
    ):
        self.chunk_id = chunk_id
        self.document_id = document_id
        self.text = text
        self.chunk_index = chunk_index
        self.metadata = metadata
    
    def to_dict(self) -> Dict[str, Any]:
        return {
            "chunk_id": self.chunk_id,
            "document_id": self.document_id,
            "text": self.text,
            "chunk_index": self.chunk_index,
            "metadata": self.metadata
        }

class DocumentProcessor:
    """
    Processes documents into knowledge chunks
    Supports: PDF, TXT, MD, DOCX
    """
    
    def __init__(
        self,
        chunk_size: int = 500,
        chunk_overlap: int = 50
    ):
        self.chunk_size = chunk_size
        self.chunk_overlap = chunk_overlap
        self.documents: Dict[str, Dict[str, Any]] = {}
        self.chunks: Dict[str, List[DocumentChunk]] = {}
    
    async def process_document(
        self,
        file_path: Path,
        filename: str
    ) -> str:
        """
        Process a document and return its ID
        
        Args:
            file_path: Path to the uploaded file
            filename: Original filename
        
        Returns:
            Document ID (UUID)
        """
        try:
            document_id = str(uuid.uuid4())
            
            # Extract text based on file type
            text = await self._extract_text(file_path)
            
            # Store document metadata
            self.documents[document_id] = {
                "document_id": document_id,
                "filename": filename,
                "file_path": str(file_path),
                "created_at": datetime.utcnow().isoformat(),
                "text_length": len(text),
                "status": "processed"
            }
            
            # Create chunks
            chunks = self._create_chunks(document_id, text, filename)
            self.chunks[document_id] = chunks
            
            logger.info(f"Processed document {filename}: {len(chunks)} chunks created")
            
            return document_id
            
        except Exception as e:
            logger.error(f"Error processing document: {e}")
            raise
    
    async def _extract_text(self, file_path: Path) -> str:
        """
        Extract text from various file formats
        """
        try:
            suffix = file_path.suffix.lower()
            
            if suffix == '.pdf':
                return await self._extract_from_pdf(file_path)
            elif suffix in ['.txt', '.md']:
                return file_path.read_text(encoding='utf-8')
            elif suffix == '.docx':
                return await self._extract_from_docx(file_path)
            else:
                raise ValueError(f"Unsupported file type: {suffix}")
                
        except Exception as e:
            logger.error(f"Error extracting text from {file_path}: {e}")
            raise
    
    async def _extract_from_pdf(self, file_path: Path) -> str:
        """Extract text from PDF"""
        try:
            text = []
            with open(file_path, 'rb') as file:
                pdf_reader = PyPDF2.PdfReader(file)
                for page in pdf_reader.pages:
                    text.append(page.extract_text())
            
            return "\n".join(text)
            
        except Exception as e:
            logger.error(f"Error reading PDF: {e}")
            raise
    
    async def _extract_from_docx(self, file_path: Path) -> str:
        """Extract text from DOCX"""
        try:
            doc = DocxDocument(file_path)
            text = [paragraph.text for paragraph in doc.paragraphs]
            return "\n".join(text)
            
        except Exception as e:
            logger.error(f"Error reading DOCX: {e}")
            raise
    
    def _create_chunks(
        self,
        document_id: str,
        text: str,
        filename: str
    ) -> List[DocumentChunk]:
        """
        Split document into overlapping chunks
        
        Args:
            document_id: Document identifier
            text: Full document text
            filename: Document filename
        
        Returns:
            List of DocumentChunk objects
        """
        chunks = []
        
        # Clean and normalize text
        text = text.replace('\n\n', ' [PARA] ').replace('\n', ' ')
        text = ' '.join(text.split())  # Normalize whitespace
        
        # Split into chunks with overlap
        start = 0
        chunk_index = 0
        
        while start < len(text):
            end = start + self.chunk_size
            chunk_text = text[start:end]
            
            # Try to break at sentence boundary
            if end < len(text):
                last_period = chunk_text.rfind('.')
                last_question = chunk_text.rfind('?')
                last_exclamation = chunk_text.rfind('!')
                
                boundary = max(last_period, last_question, last_exclamation)
                
                if boundary > self.chunk_size * 0.7:  # If boundary is reasonable
                    chunk_text = chunk_text[:boundary + 1]
                    end = start + boundary + 1
            
            # Create chunk
            chunk_id = f"{document_id}_chunk_{chunk_index}"
            
            chunk = DocumentChunk(
                chunk_id=chunk_id,
                document_id=document_id,
                text=chunk_text.strip(),
                chunk_index=chunk_index,
                metadata={
                    "filename": filename,
                    "chunk_index": chunk_index,
                    "start_pos": start,
                    "end_pos": end
                }
            )
            
            chunks.append(chunk)
            
            # Move to next chunk with overlap
            start = end - self.chunk_overlap
            chunk_index += 1
        
        return chunks
    
    async def extract_chunks(self, document_id: str) -> List[DocumentChunk]:
        """
        Retrieve chunks for a document
        
        Args:
            document_id: Document identifier
        
        Returns:
            List of DocumentChunk objects
        """
        if document_id not in self.chunks:
            raise ValueError(f"Document {document_id} not found")
        
        return self.chunks[document_id]
    
    async def delete_document(self, document_id: str):
        """
        Delete a document and its chunks
        """
        if document_id in self.documents:
            # Delete file
            file_path = Path(self.documents[document_id]["file_path"])
            if file_path.exists():
                file_path.unlink()
            
            # Remove from memory
            del self.documents[document_id]
            
            if document_id in self.chunks:
                del self.chunks[document_id]
            
            logger.info(f"Deleted document: {document_id}")
        else:
            raise ValueError(f"Document {document_id} not found")
    
    def get_document_info(self, document_id: str) -> Dict[str, Any]:
        """Get document metadata"""
        if document_id not in self.documents:
            raise ValueError(f"Document {document_id} not found")
        
        return self.documents[document_id]
    
    def list_documents(self) -> List[Dict[str, Any]]:
        """List all processed documents"""
        return list(self.documents.values())
