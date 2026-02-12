nDD server should runs cron-job everyday for monitoring/cleaning purpose
TODO:
1. Monitor disk usage (/). If more than 80% if filled in the disk, it should raise an alarm. Invoke an api end point to alert the administrator. Should only happen for serverless clusters
2. Clean journal logs of ndd which is older than 7 days
3. Clean core dumps which are older than 7 days
4. Clean nginx logs for more than 7 days. Should only happen for serverless clusters
5. Delete indexes from deleted folders which are older than 90 days
6. Monitor data disk (/mnt/data). If more than 60% is filled, it should raise an alarm.Invoke an api end point to alert the administrator. Should only happen for serverless clusters