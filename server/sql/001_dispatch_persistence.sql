-- 多列车调度系统的 MySQL 8.0 数据结构。
-- 应用可通过 mysql.auto_migrate 自动执行同等 DDL；生产环境建议由发布流程执行本文件。
CREATE TABLE IF NOT EXISTS dispatch_topology_snapshots (
  revision BIGINT UNSIGNED NOT NULL,
  received_at_wall_ms BIGINT UNSIGNED NOT NULL,
  route_count INT UNSIGNED NOT NULL,
  corridor_count INT UNSIGNED NOT NULL,
  intersection_count INT UNSIGNED NOT NULL,
  payload_json JSON NOT NULL,
  created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  PRIMARY KEY (revision), KEY idx_dispatch_topology_created (created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS dispatch_train_state (
  train_id VARCHAR(128) NOT NULL, line_id VARCHAR(128) NOT NULL,
  x DOUBLE NOT NULL, y DOUBLE NOT NULL, z DOUBLE NOT NULL, speed DOUBLE NOT NULL,
  route_index BIGINT UNSIGNED NOT NULL, travel_direction VARCHAR(16) NOT NULL,
  simulation_time_ms BIGINT UNSIGNED NOT NULL,
  received_at_wall_ms BIGINT UNSIGNED NOT NULL, phase VARCHAR(32) NOT NULL,
  controlled_resource_id VARCHAR(255) NOT NULL,
  is_active TINYINT(1) NOT NULL DEFAULT 1,
  updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)
    ON UPDATE CURRENT_TIMESTAMP(6),
  PRIMARY KEY (train_id),
  KEY idx_dispatch_train_line_active (line_id,is_active),
  KEY idx_dispatch_train_resource (controlled_resource_id,phase)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS dispatch_telemetry_batches (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  simulation_time_ms BIGINT UNSIGNED NOT NULL,
  received_at_wall_ms BIGINT UNSIGNED NOT NULL,
  train_count INT UNSIGNED NOT NULL, payload_json JSON NOT NULL,
  created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  PRIMARY KEY (id),
  KEY idx_dispatch_telemetry_simulation (simulation_time_ms,id),
  KEY idx_dispatch_telemetry_created (created_at,id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS dispatch_rolling_plans (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  topology_revision BIGINT UNSIGNED NOT NULL,
  generated_at_simulation_ms BIGINT UNSIGNED NOT NULL,
  generated_at_wall_ms BIGINT UNSIGNED NOT NULL,
  resource_count INT UNSIGNED NOT NULL, payload_json JSON NOT NULL,
  created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  PRIMARY KEY (id),
  KEY idx_dispatch_plan_revision_time
    (topology_revision,generated_at_simulation_ms,id),
  KEY idx_dispatch_plan_created (created_at,id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS dispatch_commands (
  command_id VARCHAR(128) NOT NULL,
  command_sequence BIGINT UNSIGNED NOT NULL,
  train_id VARCHAR(128) NOT NULL, corridor_resource_id VARCHAR(255) NOT NULL,
  action VARCHAR(32) NOT NULL, status VARCHAR(32) NOT NULL,
  topology_revision BIGINT UNSIGNED NOT NULL,
  issued_at_simulation_ms BIGINT UNSIGNED NOT NULL,
  issued_at_wall_ms BIGINT UNSIGNED NOT NULL,
  acknowledged_at_simulation_ms BIGINT UNSIGNED NULL,
  cleared_at_simulation_ms BIGINT UNSIGNED NULL,
  payload_json JSON NOT NULL,
  created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)
    ON UPDATE CURRENT_TIMESTAMP(6),
  PRIMARY KEY (command_id),
  UNIQUE KEY uk_dispatch_command_sequence (command_sequence),
  KEY idx_dispatch_command_train_status (train_id,status),
  KEY idx_dispatch_command_resource_status (corridor_resource_id,status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS dispatch_command_events (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  event_key VARCHAR(255) NOT NULL, command_id VARCHAR(128) NOT NULL,
  event_type VARCHAR(32) NOT NULL,
  simulation_time_ms BIGINT UNSIGNED NOT NULL,
  received_at_wall_ms BIGINT UNSIGNED NOT NULL, payload_json JSON NOT NULL,
  created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  PRIMARY KEY (id), UNIQUE KEY uk_dispatch_event_key (event_key),
  KEY idx_dispatch_event_command (command_id,id),
  CONSTRAINT fk_dispatch_event_command FOREIGN KEY (command_id)
    REFERENCES dispatch_commands(command_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
