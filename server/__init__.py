"""The Hey Claude server.

Holds one WebSocket per device and turns each spoken request into a spoken
reply: speech-to-text, Claude, then text-to-speech. See ARCHITECTURE.md.
"""
