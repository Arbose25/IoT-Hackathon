-- Generate a random 32-byte token locally. Paste ONLY its lowercase SHA-256 digest.
-- Never save the raw token in SQL, Git, or a screenshot.
begin;
do $$
declare
  digest constant text := 'REPLACE_WITH_64_LOWERCASE_HEX_DIGEST';
  school uuid;
  room uuid;
  device uuid;
begin
  if digest !~ '^[0-9a-f]{64}$' then raise exception 'Replace the digest placeholder'; end if;
  insert into public.schools(school_code,name) values('DEMO','Smart Classroom Demo')
    on conflict (school_code) do nothing;
  select id into strict school from public.schools where school_code='DEMO' and active;
  insert into public.classrooms(school_id,room_code,name) values(school,'A-101','Demo Classroom')
    on conflict (school_id,room_code) do nothing;
  select id into strict room from public.classrooms where school_id=school and room_code='A-101' and active;
  insert into public.devices(school_id,classroom_id,device_code,name,hardware_model)
    values(school,room,'DEMO-ESP32-S3-01','Demo ESP32-S3','ESP32-S3-CAM N16R8')
    on conflict (device_code) do nothing;
  select id into strict device from public.devices
    where school_id=school and classroom_id=room and device_code='DEMO-ESP32-S3-01'
      and status in ('provisioning','online','offline');
  insert into public.device_credentials(device_id,credential_label,secret_digest)
    values(device,'Level 2 ESP32 demo',digest);
end;
$$;
select id as device_uuid, device_code from public.devices where device_code='DEMO-ESP32-S3-01';
commit;
