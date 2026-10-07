"""
TuneBar AI Voice Assistant Backend Reference Implementation
============================================================
FastAPI + Groq Whisper STT + LLM (Groq/OpenAI/Anthropic) + Edge-TTS MP3 Streaming

Requirements:
    pip install fastapi uvicorn edge-tts groq requests python-multipart

Usage:
    export GROQ_API_KEY="gsk_your_groq_api_key"
    python ai_assistant_server.py --port 8000
"""

import os
import sys
import tempfile
import asyncio
from fastapi import FastAPI, UploadFile, File, Query, HTTPException
from fastapi.responses import Response, StreamingResponse
from groq import Groq
import edge_tts

app = FastAPI(title="TuneBar AI Voice Assistant API", version="2.0.0")

GROQ_API_KEY = os.environ.get("GROQ_API_KEY", "")
VOICE_NAME = os.environ.get("TTS_VOICE", "en-US-GuyNeural")
groq_client = Groq(api_key=GROQ_API_KEY) if GROQ_API_KEY else None

SYSTEM_PROMPT = (
    "You are TuneBar, an ultra-compact smart media bar voice assistant. "
    "Your answers are spoken aloud through a small 3W speaker on an ESP32-S3 device. "
    "Always respond concisely, conversationally, and accurately in 1 to 2 short sentences. "
    "Never use markdown, lists, bullet points, or code blocks."
)

async def generate_tts_stream(text: str):
    """Streams MP3 bytes in chunks directly from Microsoft Edge-TTS."""
    communicate = edge_tts.Communicate(text, VOICE_NAME)
    async for chunk in communicate.stream():
        if chunk["type"] == "audio":
            yield chunk["data"]

def ask_llm(query: str) -> str:
    """Invokes LLM (Llama 3 70B Versatile via Groq for <300ms response)."""
    if not groq_client:
        return f"Echo response: {query}"
    resp = groq_client.chat.completions.create(
        model="llama-3.3-70b-versatile",
        messages=[
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": query}
        ],
        max_tokens=150,
        temperature=0.6,
    )
    return resp.choices[0].message.content.strip()

@app.get("/ask.php")
@app.get("/api/ask")
async def ask_text(q: str = Query(..., description="Spoken or typed query string")):
    """Handles text queries: generates answer and streams MP3 audio back."""
    answer_text = ask_llm(q)
    return StreamingResponse(
        generate_tts_stream(answer_text),
        media_type="audio/mpeg",
        headers={
            "X-Query": q[:100],
            "X-Answer": answer_text[:200],
            "Content-Disposition": "inline; filename=\"response.mp3\""
        }
    )

@app.post("/ask.php")
@app.post("/api/ask")
async def ask_voice(file: UploadFile = File(...)):
    """Handles WAV voice upload from TuneBar ES7210 microphone:
    1. Transcribes audio via Groq Whisper Large V3 Turbo (<200ms).
    2. Queries LLM for concise answer (<300ms).
    3. Streams Edge-TTS MP3 audio back to device.
    """
    if not groq_client:
        raise HTTPException(status_code=500, detail="GROQ_API_KEY environment variable not set")

    # Save uploaded WAV to temporary file
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
        tmp_path = tmp.name
        content = await file.read()
        tmp.write(content)

    try:
        # Step 1: STT Transcription
        with open(tmp_path, "rb") as audio_file:
            transcript = groq_client.audio.transcriptions.create(
                file=(os.path.basename(tmp_path), audio_file.read()),
                model="whisper-large-v3-turbo",
                language="en",
                response_format="text"
            ).strip()

        if not transcript:
            transcript = "Could not hear you clearly."
            answer_text = "I did not catch that. Please try speaking again."
        else:
            # Step 2: LLM Reasoning
            answer_text = ask_llm(transcript)

        # Step 3: Stream TTS MP3 back directly in HTTP response
        return StreamingResponse(
            generate_tts_stream(answer_text),
            media_type="audio/mpeg",
            headers={
                "X-Query": transcript[:100],
                "X-Answer": answer_text[:200],
                "Content-Disposition": "inline; filename=\"response.mp3\""
            }
        )
    finally:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)

if __name__ == "__main__":
    import uvicorn
    port = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[1] == "--port" else 8000
    print(f"Starting TuneBar AI Assistant server on http://0.0.0.0:{port}")
    uvicorn.run(app, host="0.0.0.0", port=port)
