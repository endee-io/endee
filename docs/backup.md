# Backup & Restore

NDD provides a robust backup system based on filesystem snapshots. Backups are consistent and include all index data and metadata.

## APIs

### 1. Create Backup
Creates a snapshot of an existing index.

**POST** `/api/v1/index/<index_name>/backup`

```json
{
  "name": "my_backup_2024"
}
```

### 2. List Backups
List all available backups with their size and metadata.

**GET** `/api/v1/backups`

### 3. Restore Backup
Restores a backup to a **new** index. The target index must not exist.

**POST** `/api/v1/backups/<backup_name>/restore`

```json
{
  "target_index_name": "restored_index"
}
```

### 4. Delete Backup
Permanently removes a backup.

**DELETE** `/api/v1/backups/<backup_name>`

## How it works
- **Backup**: The system locks the index to ensure consistency, flushes data to disk, and copies the index directory to a backup location.
- **Restore**: The system reads the backup metadata and creates a fresh index with the same configuration, then populates it with the backup data.
