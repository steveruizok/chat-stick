-- Migration 0005: server-side store for device log lines shipped over the
-- live WebSocket (client_log messages). Lines survive abrupt device power
-- loss, so boot post-mortems / battery breadcrumbs can be read back later —
-- by the model via the read_device_logs tool, or directly with d1 execute.
-- Run with: wrangler d1 migrations apply --local
--          wrangler d1 migrations apply --remote

CREATE TABLE IF NOT EXISTS device_logs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id TEXT NOT NULL,
  line TEXT NOT NULL,
  created_at TEXT DEFAULT (datetime('now'))
);

CREATE INDEX IF NOT EXISTS idx_device_logs_device ON device_logs(device_id, id);
