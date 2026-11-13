-- Run as a database administrator. All test rows are rolled back.
begin;
do $$
declare
  school uuid := gen_random_uuid();
  room uuid := gen_random_uuid();
  device uuid := gen_random_uuid();
  other_device uuid := gen_random_uuid();
  digest text := replace(gen_random_uuid()::text,'-','') || replace(gen_random_uuid()::text,'-','');
  event_id uuid := gen_random_uuid();
  fire_event jsonb;
  payload jsonb := jsonb_build_object(
    'event_id',event_id,'boot_id',gen_random_uuid(),'sequence_no',1,'kind','telemetry',
    'captured_at',null,'clock_quality','unknown','temperature_c',27,'humidity_pct',60,
    'dht_ok',true,'rfid_ok',true,'flame',false,'watchdog_ok',true,'uptime_ms',5000,
    'wifi_rssi_dbm',-60,'free_heap_bytes',100000,'reset_reason',1,'firmware','level2');
begin
  insert into public.schools(id,school_code,name) values(school,'L2TEST-'||left(school::text,8),'Rollback test');
  insert into public.classrooms(id,school_id,room_code,name) values(room,school,'T-101','Rollback room');
  insert into public.devices(id,school_id,classroom_id,device_code,name)
    values(device,school,room,'L2TEST-'||left(device::text,8),'Test device'),
          (other_device,school,room,'L2TEST-'||left(other_device::text,8),'Other device');
  insert into public.device_credentials(device_id,credential_label,secret_digest)
    values(device,'Rollback test',digest);
  if public.classroom_ingest(device,'T-101',repeat('0',64),payload) <> 'unauthorized' or
     public.classroom_ingest(other_device,'T-101',digest,payload) <> 'unauthorized' or
     public.classroom_ingest(device,'T-102',digest,payload) <> 'unauthorized'
    then raise exception 'Invalid credential, room, or device was accepted'; end if;
  if public.classroom_ingest(device,'T-101',digest,payload) <> 'accepted'
    then raise exception 'Expected accepted'; end if;
  if public.classroom_ingest(device,'T-101',digest,payload) <> 'duplicate'
    then raise exception 'Expected duplicate'; end if;
  if public.classroom_ingest(device,'T-101',digest,jsonb_set(payload,'{event_id}',to_jsonb(gen_random_uuid()))) <> 'conflict'
    then raise exception 'Expected sequence conflict'; end if;
  if (select count(*) from public.device_events e where e.device_id=device) <> 1 or
     (select count(*) from public.sensor_readings r where r.device_id=device) <> 1 or
     (select count(*) from public.device_health h where h.device_id=device) <> 1
    then raise exception 'Unexpected row count'; end if;
  update public.devices set last_seen_at=now()-interval '40 seconds' where id=device;
  if not exists (select 1 from public.classroom_device_presence where device_id=device and presence='offline')
    then raise exception 'Expected server-side offline presence'; end if;
  fire_event := payload || jsonb_build_object(
    'event_id',gen_random_uuid(),'sequence_no',2,'firmware','level3','kind','fire',
    'heat_index_c',null,'safety','FIRE','door','UNLOCKED','fire_latched',true,'panic_latched',false);
  if public.classroom_ingest(device,'T-101',digest,fire_event) <> 'accepted' or
     public.classroom_ingest(device,'T-101',digest,fire_event) <> 'duplicate'
    then raise exception 'Fire replay was not idempotent'; end if;
  if (select count(*) from public.alerts a where a.device_id=device and alert_type='fire') <> 1 or
     (select count(*) from public.safety_state_history h where h.device_id=device and fire_latched) <> 1 or
     (select count(*) from public.outage_sessions o where o.device_id=device) <> 1
    then raise exception 'Expected one alert, latch record and reconciled outage'; end if;
  fire_event := fire_event || jsonb_build_object('event_id',gen_random_uuid(),'sequence_no',3,
    'kind','fire_reset','safety','SAFE','door','LOCKED','fire_latched',false);
  if public.classroom_ingest(device,'T-101',digest,fire_event) <> 'accepted'
    then raise exception 'Expected accepted fire reset'; end if;
  if not exists (select 1 from public.alerts a where a.device_id=device and status='resolved')
    then raise exception 'Expected resolved fire alert'; end if;
  fire_event := fire_event || jsonb_build_object('event_id',gen_random_uuid(),'sequence_no',4,
    'kind','panic','safety','PANIC','panic_latched',true);
  if public.classroom_ingest(device,'T-101',digest,fire_event) <> 'accepted'
    then raise exception 'Expected accepted panic event'; end if;
  if not exists (select 1 from public.alerts a where a.device_id=device
                 and alert_type='panic' and severity='critical')
    then raise exception 'Expected critical panic alert'; end if;
  update public.device_credentials set revoked_at=now() where device_id=device;
  if public.classroom_ingest(device,'T-101',digest,payload) <> 'unauthorized'
    then raise exception 'Revoked credential was accepted'; end if;
end;
$$;
rollback;
