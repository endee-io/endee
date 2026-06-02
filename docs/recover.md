> **Legacy procedure.** This document describes the pre-`single_txn` manual recovery flow that ran against the old per-component MDBX layout (v0). On the current shared-env layout (v2), recovery is automatic: `IndexManager::recoverFromWAL` replays committed `op_log` rows on index load and saves+clears the WAL idempotently. See [docs/mdbx_shared_env_acid_revamp.md](mdbx_shared_env_acid_revamp.md) § HNSW Recovery.
>
> The flow below still applies if you have a layout-v0 index that has not been migrated. To migrate, see [docs/migrator.md](migrator.md).

The recovery feature recovers from the legacy split MDBX envs and recreates the index. Below are the steps to recover. It tracks the progress with a file recover.txt. It reads the vector store MDBX env and sends key (numeric_id) and value vector to the hnsw index for index creation.
1. Rename the index file
```
mv main.idx main.idx.old
```
2. Make sure that there are no uploads otherwise it may corrupt the index
3. Post a reset command which will generate a blank main.idx and recover.txt in the folder to track the batch progress. Make sure the the parameters matches the original parameters of the index

```shell
curl --request POST \
  --url https://iw1.endee.io/api/v1/admin/users/k1ll6e5w/indices/pubmed_collection_v1/reset \
  --header 'authorization: {{root_token}}' \
  --header 'content-type: application/json' \
  --data '{"dim":1024,
"space_type":"cosine"}'
```
4. Run recover.sh. It will keep recovering in loop

```shell
while true; do
  http_code=$(curl -s -o /dev/null -w "%{http_code}" --request POST \
    --url https://iw1.endee.io/api/v1/admin/users/<username>/indices/<indexname>/recover \
    --header 'Authorization: root:19cBoY9mjweKk1FfvewZo415pj3VzXXS')

  echo "Response code: $http_code"

  if [ "$http_code" -ne 200 ]; then
    echo "Non-200 response received. Exiting..."
    break
  fi
done
```
5. Delete recover.txt after the index creation
```shell
rm recover.txt
```
TODO:
1. When resetting the index if the fp16 parameter is wrong it may crash the index. Maybe we should read the parameter from the indices MDBX env and verify while resetting.