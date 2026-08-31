-- Migration 0004: group image rows into flipbook animations for show_animation
-- Frames of one animation share an animation_group (uuid) and are ordered by
-- frame_index (0-based). Plain images leave both columns NULL.
-- Run with: wrangler d1 migrations apply --local
--          wrangler d1 migrations apply --remote

ALTER TABLE images ADD COLUMN animation_group TEXT;
ALTER TABLE images ADD COLUMN frame_index INTEGER;

CREATE INDEX IF NOT EXISTS idx_images_animation_group ON images(device_id, animation_group);
