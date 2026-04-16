-- D1 schema for M5 Live voice assistant
-- Conversations are tracked by chat_id (client-generated), owned by device_id

CREATE TABLE IF NOT EXISTS conversations (
  chat_id TEXT PRIMARY KEY,
  device_id TEXT NOT NULL,
  messages TEXT NOT NULL DEFAULT '[]',  -- JSON array of {role, content}
  last_message TEXT,                     -- last AI response (for boot restore)
  created_at TEXT DEFAULT (datetime('now')),
  updated_at TEXT DEFAULT (datetime('now'))
);

CREATE INDEX IF NOT EXISTS idx_conversations_device
  ON conversations(device_id, updated_at);

-- Message log: records each exchange for telemetry
CREATE TABLE IF NOT EXISTS message_log (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id TEXT NOT NULL,
  chat_id TEXT NOT NULL,
  user_text TEXT,
  assistant_text TEXT,
  created_at TEXT DEFAULT (datetime('now'))
);

CREATE INDEX IF NOT EXISTS idx_message_log_device ON message_log(device_id);

-- Tool call log: every function call the model makes
CREATE TABLE IF NOT EXISTS tool_log (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id TEXT NOT NULL,
  chat_id TEXT NOT NULL,
  tool_name TEXT NOT NULL,
  args TEXT,                             -- JSON
  result TEXT,                           -- JSON or string
  handled_by TEXT NOT NULL,              -- 'server' | 'device'
  status TEXT NOT NULL DEFAULT 'ok',     -- 'ok' | 'error'
  error TEXT,
  duration_ms INTEGER,
  created_at TEXT DEFAULT (datetime('now'))
);

CREATE INDEX IF NOT EXISTS idx_tool_log_device ON tool_log(device_id, created_at);
CREATE INDEX IF NOT EXISTS idx_tool_log_chat ON tool_log(chat_id);
