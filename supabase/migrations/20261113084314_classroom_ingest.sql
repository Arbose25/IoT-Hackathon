begin;

create function public.classroom_ingest(
  p_device uuid, p_room text, p_digest text, p jsonb
) returns text
language plpgsql security invoker set search_path = ''
as $$
declare
  device public.devices%rowtype;
  credential uuid;
  event_row uuid;
  event_uuid uuid := (p->>'event_id')::uuid;
  boot_uuid uuid := (p->>'boot_id')::uuid;
  seq bigint := (p->>'sequence_no')::bigint;
  captured timestamptz := (p->>'captured_at')::timestamptz;
  received timestamptz := clock_timestamp();
  kind text := p->>'kind';
begin
  -- Serialize authorization, deduplication, heartbeat updates and rate limits.
  select d.* into device from public.devices d
    join public.classrooms c on c.id = d.classroom_id and c.school_id = d.school_id
    join public.schools s on s.id = d.school_id
    where d.id = p_device and c.room_code = p_room and c.active and s.active
      and d.status in ('provisioning','online','offline')
    for update of d;
  if not found then return 'unauthorized'; end if;
  select id into credential from public.device_credentials
    where device_id = p_device and secret_digest = p_digest and revoked_at is null
      and (expires_at is null or expires_at > received)
    for update;
  if not found then return 'unauthorized'; end if;
  if (select count(*) from public.device_events where device_id = p_device
      and received_at >= received - interval '10 seconds') >= 40 then
    return 'limited';
  end if;

  insert into public.device_events
    (device_id,event_id,boot_id,sequence_no,event_type,severity,payload,occurred_at,clock_quality,received_at)
  values (p_device,event_uuid,boot_uuid,seq,kind,'info',p,captured,p->>'clock_quality',received)
  on conflict do nothing returning id into event_row;
  if event_row is null then
    if exists (select 1 from public.device_events where device_id = p_device
      and event_id = event_uuid and boot_id = boot_uuid and sequence_no = seq and payload = p)
    then return 'duplicate'; end if;
    return 'conflict';
  end if;

  if kind = 'telemetry' then
    insert into public.sensor_readings
      (device_id,event_id,boot_id,sequence_no,captured_at,clock_quality,received_at,temperature_c,
       humidity_pct,flame_detected,dht_ok,rfid_ok,wifi_rssi_dbm)
    values (p_device,event_uuid,boot_uuid,seq,captured,p->>'clock_quality',received,
      (p->>'temperature_c')::numeric,(p->>'humidity_pct')::numeric,(p->>'flame')::boolean,
      (p->>'dht_ok')::boolean,(p->>'rfid_ok')::boolean,(p->>'wifi_rssi_dbm')::smallint);
    insert into public.device_health
      (device_id,boot_id,uptime_ms,free_heap_bytes,watchdog_ok,dht_ok,rfid_ok,recorded_at,received_at)
    values (p_device,boot_uuid,(p->>'uptime_ms')::bigint,(p->>'free_heap_bytes')::bigint,
      (p->>'watchdog_ok')::boolean,(p->>'dht_ok')::boolean,(p->>'rfid_ok')::boolean,captured,received);
  elsif kind = 'boot' then
    insert into public.device_boots (device_id,boot_id,reset_reason,firmware_version,booted_at,received_at)
    values (p_device,boot_uuid,p->>'reset_reason',p->>'firmware',captured,received)
    on conflict (device_id,boot_id) do nothing;
  else
    raise exception 'Unsupported event type';
  end if;

  update public.devices set last_seen_at = received, status = 'online',
    firmware_version = p->>'firmware' where id = p_device;
  update public.device_credentials set last_used_at = received where id = credential;
  return 'accepted';
end;
$$;

revoke all on function public.classroom_ingest(uuid,text,text,jsonb) from public, anon, authenticated;
grant execute on function public.classroom_ingest(uuid,text,text,jsonb) to service_role;
commit;
