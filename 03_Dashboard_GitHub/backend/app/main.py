"""FastAPI application serving one local Smart Eco Garden dashboard."""
from __future__ import annotations
import asyncio
from contextlib import asynccontextmanager
from fastapi import FastAPI
from app.eco_demo import router, polling_task, demo_tick_task

@asynccontextmanager
async def lifespan(app: FastAPI):
    tasks = [asyncio.create_task(polling_task()), asyncio.create_task(demo_tick_task())]
    try:
        yield
    finally:
        for task in tasks: task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)

app = FastAPI(title="Smart Eco Garden API", version="1.0", lifespan=lifespan)
app.include_router(router)

@app.get('/api/health')
def health():
    return {'ok': True, 'system': 'Smart Eco Garden', 'role': 'FastAPI backend'}
