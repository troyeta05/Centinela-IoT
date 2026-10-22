-- CENTINELA: esquema completo para pegar en el SQL Editor de Supabase (01 a 05)

-- =====================================================================
-- CENTINELA - 01_schema.sql
-- Esquema base (version corregida para empatar con el JSON v1.1)
-- Correr en Supabase: SQL Editor. Orden: 01 -> 02 -> 03 -> 04
-- =====================================================================
-- Nota: gen_random_uuid() ya viene en Postgres 13+, no hace falta uuid-ossp.

-----------------------------------------------------------------
-- 1. DOMICILIOS Y MEMBRESIAS
-----------------------------------------------------------------
create table if not exists public.homes (
    id uuid primary key default gen_random_uuid(),
    name text not null check (length(trim(name)) > 0),
    timezone text not null default 'America/Mexico_City',
    created_at timestamptz not null default now()
);

create table if not exists public.memberships (
    home_id uuid not null references public.homes(id) on delete restrict,
    user_id uuid not null references auth.users(id) on delete restrict,
    role text not null check (role in ('reader', 'technician')),
    status text not null check (status in ('active', 'inactive')),
    primary key (home_id, user_id)
);

-----------------------------------------------------------------
-- 2. ROBOTS, DISPOSITIVOS Y SESIONES
-----------------------------------------------------------------
create table if not exists public.robots (
    id uuid primary key default gen_random_uuid(),
    home_id uuid not null references public.homes(id) on delete restrict,
    public_code text not null unique,
    last_stream_version bigint not null default 0 check (last_stream_version >= 0),
    status text not null default 'active'
);

-- devices.id es el "device_id" que viaja en el JSON
create table if not exists public.devices (
    id uuid primary key default gen_random_uuid(),
    robot_id uuid not null references public.robots(id) on delete restrict,
    public_id text not null unique,
    status text not null default 'active',
    firmware_version text,
    last_seen_at timestamptz
);

create table if not exists public.device_sessions (
    id uuid primary key default gen_random_uuid(),
    device_id uuid not null references public.devices(id) on delete restrict,
    boot_id uuid not null,
    started_at timestamptz not null default now(),
    clock_quality text not null default 'unknown',
    unique (device_id, boot_id)
);

-----------------------------------------------------------------
-- 3. MAPAS Y MISIONES
-----------------------------------------------------------------
create table if not exists public.map_versions (
    id uuid primary key default gen_random_uuid(),
    home_id uuid not null references public.homes(id) on delete restrict,
    version_no int not null,
    status text not null check (status in ('draft', 'validated', 'retired')),
    resolution_m numeric not null default 0.05,
    origin_x numeric not null default 0,
    origin_y numeric not null default 0,
    width int not null,
    height int not null,
    asset_path text not null,
    checksum text not null,
    unique (home_id, version_no)
);

create table if not exists public.missions (
    id uuid primary key default gen_random_uuid(),
    robot_id uuid not null references public.robots(id) on delete restrict,
    map_version_id uuid not null references public.map_versions(id) on delete restrict,
    started_at timestamptz not null default now(),
    ended_at timestamptz,
    status text not null,
    stop_reason text
);

-----------------------------------------------------------------
-- 4. TELEMETRIA
-----------------------------------------------------------------
create table if not exists public.telemetry_frames (
    id uuid primary key,                       -- = event_id del JSON
    robot_id uuid not null references public.robots(id) on delete restrict,
    device_session_id uuid not null references public.device_sessions(id) on delete restrict,
    mission_id uuid references public.missions(id) on delete set null,
    sequence bigint not null check (sequence >= 0),
    captured_at timestamptz,
    received_at timestamptz not null default now(),
    monotonic_ms bigint not null check (monotonic_ms >= 0),
    clock_quality text not null,
    stream_version bigint not null,
    payload_hash text not null,
    schema_version text,                       -- NUEVO: version del JSON recibido
    raw_payload jsonb,                         -- NUEVO: JSON original, para auditoria
    unique (device_session_id, sequence),
    unique (robot_id, stream_version)
);

-- Catalogo de canales conocidos (NUEVO): define tipo y unidad de cada channel_key
create table if not exists public.channel_catalog (
    channel_key text primary key,
    metric_type text not null,
    unit_code text not null,
    description text
);

create table if not exists public.channels (
    id uuid primary key default gen_random_uuid(),
    robot_id uuid not null references public.robots(id) on delete restrict,
    channel_key text not null,
    metric_type text not null,
    unit_code text not null,
    unique (robot_id, channel_key)
);

create table if not exists public.sensor_readings (
    frame_id uuid not null references public.telemetry_frames(id) on delete cascade,
    channel_id uuid not null references public.channels(id) on delete restrict,
    value double precision,
    boolean_value boolean,
    quality text not null,
    calibration_id uuid,
    primary key (frame_id, channel_id),
    check (
        (value is not null and boolean_value is null) or
        (value is null and boolean_value is not null) or
        (value is null and boolean_value is null)
    )
);

-- CAMBIO: map_version_id ahora acepta null (el robot puede mandar pose sin mapa)
create table if not exists public.frame_poses (
    frame_id uuid primary key references public.telemetry_frames(id) on delete cascade,
    map_version_id uuid references public.map_versions(id) on delete restrict,
    x_m double precision not null,
    y_m double precision not null,
    theta_rad double precision not null,
    sigma_position_m double precision check (sigma_position_m >= 0),
    sigma_heading_rad double precision check (sigma_heading_rad >= 0),
    quality text not null
);

-- CAMBIO: navigation_state con default (el JSON solo manda "robot_state")
create table if not exists public.robot_states (
    frame_id uuid primary key references public.telemetry_frames(id) on delete cascade,
    navigation_state text not null default 'unknown',
    reported_state text not null,
    motors_active boolean,
    wifi_rssi_dbm int
);

create table if not exists public.battery_readings (
    frame_id uuid primary key references public.telemetry_frames(id) on delete cascade,
    voltage_v double precision check (voltage_v >= 0),
    current_a double precision,
    soc_pct numeric check (soc_pct >= 0 and soc_pct <= 100),
    method_code text not null,
    quality text not null
);

-----------------------------------------------------------------
-- 5. ALERTAS
-----------------------------------------------------------------
create table if not exists public.alerts (
    id uuid primary key default gen_random_uuid(),
    robot_id uuid not null references public.robots(id) on delete restrict,
    type text not null,
    severity text not null check (severity in ('info', 'warning', 'critical')),
    state text not null check (state in ('open', 'acknowledged', 'resolved')),
    origin text not null check (origin in ('device_reported', 'backend_rule')),
    opened_at timestamptz not null default now(),
    last_seen_at timestamptz not null default now(),
    resolved_at timestamptz,
    source_frame_id uuid references public.telemetry_frames(id) on delete set null, -- NUEVO
    details jsonb                                                                   -- NUEVO
);

-- Una sola alerta viva por robot y tipo (evita spam de alertas repetidas)
create unique index if not exists alerts_one_live_per_type
    on public.alerts(robot_id, type) where state in ('open', 'acknowledged');

-----------------------------------------------------------------
-- 6. INDICES
-----------------------------------------------------------------
create index if not exists frames_capture_idx on public.telemetry_frames(robot_id, captured_at desc);
create index if not exists frames_received_idx on public.telemetry_frames(robot_id, received_at desc);
create index if not exists readings_channel_idx on public.sensor_readings(channel_id, frame_id);
create index if not exists alerts_robot_state_idx on public.alerts(robot_id, state);

-----------------------------------------------------------------
-- 7. CATALOGO INICIAL DE CANALES (los que manda el firmware 0.2.0)
-----------------------------------------------------------------
insert into public.channel_catalog (channel_key, metric_type, unit_code, description) values
    ('mq6_raw',              'gas_raw',   'adc',     'MQ-6 (LP/propano-butano), lectura ADC cruda'),
    ('mq4_raw',              'gas_raw',   'adc',     'MQ-4 (metano/gas natural), lectura ADC cruda'),
    ('mq7_raw',              'gas_raw',   'adc',     'MQ-7 (monoxido de carbono), lectura ADC cruda'),
    ('temperature',          'temperature','celsius','DHT22 temperatura'),
    ('humidity',             'humidity',  'percent', 'DHT22 humedad relativa'),
    ('front_distance_m',     'distance',  'meter',   'HC-SR04 distancia frontal'),
    ('ir_floor_front_left',  'digital',   'bool',    'IR piso frente izquierdo'),
    ('ir_floor_front_right', 'digital',   'bool',    'IR piso frente derecho'),
    ('ir_floor_rear_left',   'digital',   'bool',    'IR piso atras izquierdo'),
    ('ir_floor_rear_right',  'digital',   'bool',    'IR piso atras derecho'),
    ('ir_lateral_left',      'digital',   'bool',    'IR lateral izquierdo (pared)'),
    ('ir_lateral_right',     'digital',   'bool',    'IR lateral derecho (pared)')
on conflict (channel_key) do nothing;

-- =====================================================================
-- CENTINELA - 02_calibracion.sql
-- Interpretacion de gases (MQ) en el BACKEND. El robot solo manda ADC crudo.
-- Web y movil NO calculan nada: consultan las vistas del archivo 04.
-- =====================================================================

-- Calibraciones versionadas. robot_id NULL = valores por defecto para cualquier robot.
create table if not exists public.gas_calibrations (
    id uuid primary key default gen_random_uuid(),
    robot_id uuid references public.robots(id) on delete restrict,
    channel_key text not null references public.channel_catalog(channel_key),
    gas_label text not null,
    method text not null default 'mq_power_law',
    -- default         = curva de referencia, SIN R0 (solo muestra senal relativa)
    -- field_clean_air = R0 medido en aire limpio con el propio robot (estimacion)
    -- lab_validated   = calibrado contra gas patron (valor confiable)
    status text not null check (status in ('default', 'field_clean_air', 'lab_validated', 'retired')),
    params jsonb not null,
    note text,
    created_at timestamptz not null default now()
);
create index if not exists gas_cal_lookup_idx on public.gas_calibrations(channel_key, robot_id, status);

-- ---------------------------------------------------------------------
-- Parametros (jsonb):
--   adc_max        resolucion del ADC (ESP32 = 4095)
--   adc_vref       voltaje de referencia del ADC (ESP32 ~ 3.3)
--   divider_ratio  factor del divisor de voltaje del pin AO (1.0 si va directo)
--   vcc            alimentacion del sensor (5.0)
--   rl_kohm        resistencia de carga del modulo (10 en la mayoria)
--   r0_kohm        Rs en aire limpio / clean_air_ratio (null = sin calibrar)
--   clean_air_ratio  Rs/R0 en aire limpio segun datasheet
--   a, b           curva ppm = a * (Rs/R0)^b
--   max_ppm        tope del rango del sensor
--   warn_ppm, crit_ppm  umbrales de alerta (propuesta: 5% y 10% del LIE)
-- OJO: adc_vref, divider_ratio y rl_kohm deben coincidir con el circuito real.
-- ---------------------------------------------------------------------
insert into public.gas_calibrations (robot_id, channel_key, gas_label, status, params, note)
select null, v.channel_key, v.gas_label, 'default', v.params::jsonb, v.note
from (values
    ('mq6_raw', 'LP (propano/butano)',
     '{"adc_max":4095,"adc_vref":3.3,"divider_ratio":1.0,"vcc":5.0,"rl_kohm":10,"r0_kohm":null,"clean_air_ratio":10,"a":1009.2,"b":-2.35,"max_ppm":10000,"warn_ppm":1000,"crit_ppm":2100}',
     'Curva de referencia (libreria MQUnifiedsensor). Umbrales: ~5% y 10% del LIE del propano.'),
    ('mq4_raw', 'Metano / gas natural',
     '{"adc_max":4095,"adc_vref":3.3,"divider_ratio":1.0,"vcc":5.0,"rl_kohm":10,"r0_kohm":null,"clean_air_ratio":4.4,"a":1012.7,"b":-2.786,"max_ppm":10000,"warn_ppm":2500,"crit_ppm":5000}',
     'Curva de referencia (libreria MQUnifiedsensor). Umbrales: ~5% y 10% del LIE del metano.'),
    ('mq7_raw', 'Monoxido de carbono',
     '{"adc_max":4095,"adc_vref":3.3,"divider_ratio":1.0,"vcc":5.0,"rl_kohm":10,"r0_kohm":null,"clean_air_ratio":27.5,"a":99.042,"b":-1.518,"max_ppm":2000,"warn_ppm":35,"crit_ppm":100}',
     'MQ-7 necesita ciclo de calentado (5V/1.4V); sin ese ciclo la estimacion no es confiable.')
) as v(channel_key, gas_label, params, note)
where not exists (
    select 1 from public.gas_calibrations g
    where g.robot_id is null and g.channel_key = v.channel_key and g.status = 'default'
);

-- ---------------------------------------------------------------------
-- Calibracion elegida para un robot + canal (la mas confiable y reciente)
-- ---------------------------------------------------------------------
create or replace function public.calibration_for(p_robot_id uuid, p_channel_key text)
returns public.gas_calibrations
language sql stable
set search_path = public
as $$
    select g.*
    from public.gas_calibrations g
    where g.channel_key = p_channel_key
      and g.status <> 'retired'
      and (g.robot_id = p_robot_id or g.robot_id is null)
    order by (g.robot_id is not null) desc,
             case g.status when 'lab_validated' then 3 when 'field_clean_air' then 2 else 1 end desc,
             g.created_at desc
    limit 1;
$$;

-- ---------------------------------------------------------------------
-- Estimacion MQ: ADC crudo -> senal relativa, Rs y ppm estimados
-- ppm solo sale si hay R0 (calibracion). Sin R0 solo hay senal relativa.
-- ---------------------------------------------------------------------
create or replace function public.mq_estimate(p_raw double precision, p jsonb)
returns table (signal_pct double precision, rs_kohm double precision, ratio double precision, ppm double precision)
language plpgsql immutable
as $$
declare
    v_adc_max double precision := coalesce((p->>'adc_max')::double precision, 4095);
    v_vref    double precision := coalesce((p->>'adc_vref')::double precision, 3.3);
    v_div     double precision := coalesce((p->>'divider_ratio')::double precision, 1.0);
    v_vcc     double precision := coalesce((p->>'vcc')::double precision, 5.0);
    v_rl      double precision := coalesce((p->>'rl_kohm')::double precision, 10);
    v_r0      double precision := (p->>'r0_kohm')::double precision;
    v_a       double precision := (p->>'a')::double precision;
    v_b       double precision := (p->>'b')::double precision;
    v_max     double precision := coalesce((p->>'max_ppm')::double precision, 10000);
    v_vs      double precision;
begin
    signal_pct := null; rs_kohm := null; ratio := null; ppm := null;
    if p_raw is not null then
        signal_pct := least(greatest(p_raw / v_adc_max * 100, 0), 100);
        v_vs := p_raw / v_adc_max * v_vref * v_div;
        if v_vs > 0.001 and v_vs < v_vcc then
            rs_kohm := v_rl * (v_vcc - v_vs) / v_vs;
            if v_r0 is not null and v_r0 > 0 and v_a is not null and v_b is not null then
                ratio := rs_kohm / v_r0;
                ppm := least(v_a * power(ratio, v_b), v_max);
            end if;
        end if;
    end if;
    return next;
end;
$$;

-- ---------------------------------------------------------------------
-- Nivel de riesgo a partir de ppm estimados y umbrales
-- ---------------------------------------------------------------------
create or replace function public.gas_level(p_ppm double precision, p jsonb)
returns text
language sql immutable
as $$
    select case
        when p_ppm is null then null
        when p_ppm >= (p->>'crit_ppm')::double precision then 'critical'
        when p_ppm >= (p->>'warn_ppm')::double precision then 'warning'
        else 'normal'
    end;
$$;

-- ---------------------------------------------------------------------
-- Calibracion en aire limpio: el robot se deja ~10 min en un cuarto ventilado
-- y esta funcion fija R0 a partir de las lecturas recientes.
-- Uso:  select public.calibrate_r0_clean_air('<robot_id>', 'mq6_raw');
-- ---------------------------------------------------------------------
create or replace function public.calibrate_r0_clean_air(
    p_robot_id uuid,
    p_channel_key text,
    p_window_minutes int default 10,
    p_min_samples int default 10,
    p_note text default null
) returns uuid
language plpgsql
set search_path = public
as $$
declare
    v_base public.gas_calibrations;
    v_avg double precision;
    v_n bigint;
    v_rs double precision;
    v_ratio double precision;
    v_r0 double precision;
    v_new uuid;
begin
    v_base := public.calibration_for(p_robot_id, p_channel_key);
    if v_base.id is null then
        raise exception 'No hay calibracion base para el canal %', p_channel_key;
    end if;

    -- mediana (no promedio): un picon de gas durante la calibracion no la arruina
    select percentile_cont(0.5) within group (order by sr.value), count(*) into v_avg, v_n
    from public.sensor_readings sr
    join public.channels c on c.id = sr.channel_id
    join public.telemetry_frames f on f.id = sr.frame_id
    where c.robot_id = p_robot_id
      and c.channel_key = p_channel_key
      and sr.value is not null
      and sr.quality in ('uncalibrated', 'valid')
      and f.received_at >= now() - make_interval(mins => p_window_minutes);

    if v_n < p_min_samples then
        raise exception 'Muestras insuficientes para %: % de % requeridas', p_channel_key, v_n, p_min_samples;
    end if;

    select e.rs_kohm into v_rs from public.mq_estimate(v_avg, v_base.params) e;
    v_ratio := (v_base.params->>'clean_air_ratio')::double precision;
    if v_rs is null or v_ratio is null or v_ratio <= 0 then
        raise exception 'No se pudo calcular Rs (revisar adc_vref/divider_ratio). Promedio ADC=%', v_avg;
    end if;
    v_r0 := v_rs / v_ratio;

    update public.gas_calibrations
       set status = 'retired'
     where robot_id = p_robot_id and channel_key = p_channel_key and status = 'field_clean_air';

    insert into public.gas_calibrations (robot_id, channel_key, gas_label, status, params, note)
    values (p_robot_id, p_channel_key, v_base.gas_label, 'field_clean_air',
            v_base.params || jsonb_build_object('r0_kohm', v_r0),
            coalesce(p_note, format('R0 en aire limpio: %s muestras, ADC mediana %s', v_n, round(v_avg::numeric, 1))))
    returning id into v_new;
    return v_new;
end;
$$;

-- =====================================================================
-- CENTINELA - 03_ingesta.sql
-- Punto unico de entrada de telemetria:  select public.ingest_telemetry('<json>'::jsonb);
-- Desde supabase-js:  supabase.rpc('ingest_telemetry', { p: payload })   (con service_role)
-- Hace lo que el ESP32 NO debe hacer: resolver robot, sesion, stream_version,
-- hash, idempotencia, alta de canales, pose, estado, bateria y alertas de gas.
-- =====================================================================
create or replace function public.ingest_telemetry(p jsonb)
returns jsonb
language plpgsql
security definer
set search_path = public
as $$
declare
    v_event      uuid;
    v_device     uuid;
    v_robot      uuid;
    v_home       uuid;
    v_session    uuid;
    v_hash       text;
    v_old_hash   text;
    v_stream     bigint;
    v_mission    uuid;
    v_map        uuid;
    v_n_reads    int;
    v_alerts     jsonb := '[]'::jsonb;
    r            record;
    v_cal        public.gas_calibrations;
    v_est        record;
    v_level      text;
    v_type       text;
begin
    -- 1. Validacion minima del contrato
    if p is null or jsonb_typeof(p) <> 'object' then
        raise exception 'payload no es un objeto JSON';
    end if;
    if not (p ? 'event_id' and p ? 'device_id' and p ? 'boot_id' and p ? 'sequence'
            and p ? 'monotonic_ms' and p ? 'clock_quality' and p ? 'readings') then
        raise exception 'faltan campos obligatorios (event_id, device_id, boot_id, sequence, monotonic_ms, clock_quality, readings)';
    end if;
    if jsonb_typeof(p->'readings') <> 'array' then
        raise exception 'readings debe ser un arreglo';
    end if;

    v_event := (p->>'event_id')::uuid;
    v_hash  := encode(sha256(convert_to(p::text, 'UTF8')), 'hex');

    -- 2. Idempotencia: el mismo event_id no se guarda dos veces
    select payload_hash into v_old_hash from public.telemetry_frames where id = v_event;
    if found then
        return jsonb_build_object(
            'status', case when v_old_hash = v_hash then 'duplicate' else 'conflict' end,
            'frame_id', v_event);
    end if;

    -- 3. device_id (JSON) -> robot_id
    select d.id, d.robot_id, rb.home_id into v_device, v_robot, v_home
    from public.devices d
    join public.robots rb on rb.id = d.robot_id
    where d.id = (p->>'device_id')::uuid and d.status = 'active';
    if v_device is null then
        raise exception 'device_id desconocido o inactivo: %', p->>'device_id';
    end if;

    -- 4. boot_id -> sesion del dispositivo (se crea si el robot acaba de reiniciar)
    insert into public.device_sessions (device_id, boot_id, clock_quality)
    values (v_device, (p->>'boot_id')::uuid, p->>'clock_quality')
    on conflict (device_id, boot_id) do update set clock_quality = excluded.clock_quality
    returning id into v_session;

    if exists (select 1 from public.telemetry_frames
               where device_session_id = v_session and sequence = (p->>'sequence')::bigint) then
        return jsonb_build_object('status', 'duplicate_sequence',
                                  'sequence', (p->>'sequence')::bigint);
    end if;

    -- 5. Referencias opcionales: si la mision/mapa no existen en la nube se guarda null
    select m.id into v_mission from public.missions m
     where m.id = nullif(p->>'mission_id', '')::uuid and m.robot_id = v_robot;
    select mv.id into v_map from public.map_versions mv
     where mv.id = nullif(p->>'map_version_id', '')::uuid and mv.home_id = v_home;

    -- 6. stream_version consecutivo por robot (lo asigna la nube, no el robot)
    update public.robots set last_stream_version = last_stream_version + 1
     where id = v_robot returning last_stream_version into v_stream;

    -- 7. Frame
    insert into public.telemetry_frames
        (id, robot_id, device_session_id, mission_id, sequence, captured_at, monotonic_ms,
         clock_quality, stream_version, payload_hash, schema_version, raw_payload)
    values
        (v_event, v_robot, v_session, v_mission, (p->>'sequence')::bigint,
         nullif(p->>'captured_at', '')::timestamptz, (p->>'monotonic_ms')::bigint,
         p->>'clock_quality', v_stream, v_hash, p->>'schema_version', p);

    -- 8. Lecturas (canales nuevos se dan de alta solos)
    insert into public.channels (robot_id, channel_key, metric_type, unit_code)
    select distinct v_robot, x->>'channel_key',
           coalesce(cc.metric_type, 'unknown'), coalesce(cc.unit_code, 'unknown')
    from jsonb_array_elements(p->'readings') x
    left join public.channel_catalog cc on cc.channel_key = x->>'channel_key'
    on conflict (robot_id, channel_key) do nothing;

    insert into public.sensor_readings (frame_id, channel_id, value, boolean_value, quality, calibration_id)
    select v_event, c.id,
           (x->>'value')::double precision,
           (x->>'boolean_value')::boolean,
           coalesce(x->>'quality', 'unknown'),
           nullif(x->>'calibration_id', '')::uuid
    from jsonb_array_elements(p->'readings') x
    join public.channels c on c.robot_id = v_robot and c.channel_key = x->>'channel_key';
    get diagnostics v_n_reads = row_count;

    -- 9. Pose
    if jsonb_typeof(p->'pose') = 'object' then
        insert into public.frame_poses
            (frame_id, map_version_id, x_m, y_m, theta_rad, sigma_position_m, sigma_heading_rad, quality)
        values
            (v_event, v_map,
             (p->'pose'->>'x_m')::double precision,
             (p->'pose'->>'y_m')::double precision,
             (p->'pose'->>'theta_rad')::double precision,
             (p->'pose'->>'sigma_position_m')::double precision,
             (p->'pose'->>'sigma_heading_rad')::double precision,
             coalesce(p->'pose'->>'quality', 'unknown'));
    end if;

    -- 10. Estado del robot
    insert into public.robot_states (frame_id, navigation_state, reported_state, motors_active, wifi_rssi_dbm)
    values (v_event,
            coalesce(p->>'navigation_state', 'unknown'),
            coalesce(p->>'robot_state', 'unknown'),
            (p->>'motors_active')::boolean,
            (p->>'wifi_rssi_dbm')::int);

    -- 11. Bateria
    if jsonb_typeof(p->'battery') = 'object' then
        insert into public.battery_readings (frame_id, voltage_v, current_a, soc_pct, method_code, quality)
        values (v_event,
                (p->'battery'->>'voltage_v')::double precision,
                (p->'battery'->>'current_a')::double precision,
                (p->'battery'->>'soc_pct')::numeric,
                coalesce(p->'battery'->>'method_code', p->'battery'->>'quality', 'unknown'),
                coalesce(p->'battery'->>'quality', 'unknown'));
    end if;

    -- 12. Dispositivo vivo
    update public.devices
       set last_seen_at = now(),
           firmware_version = coalesce(p->>'firmware_version', firmware_version)
     where id = v_device;

    -- 13. Reglas de gas en backend (solo si hay calibracion con R0 -> ppm estimados)
    for r in
        select c.channel_key, sr.value
        from public.sensor_readings sr
        join public.channels c on c.id = sr.channel_id
        where sr.frame_id = v_event and c.metric_type = 'gas_raw'
          and sr.value is not null and sr.quality in ('uncalibrated', 'valid')
    loop
        v_cal := public.calibration_for(v_robot, r.channel_key);
        continue when v_cal.id is null;
        select * into v_est from public.mq_estimate(r.value, v_cal.params);
        continue when v_est.ppm is null;
        v_level := public.gas_level(v_est.ppm, v_cal.params);
        v_type  := 'gas_' || replace(r.channel_key, '_raw', '');

        if v_level in ('warning', 'critical') then
            insert into public.alerts (robot_id, type, severity, state, origin, source_frame_id, details)
            values (v_robot, v_type, v_level, 'open', 'backend_rule', v_event,
                    jsonb_build_object('gas', v_cal.gas_label, 'ppm_estimated', round(v_est.ppm::numeric, 1),
                                       'raw_adc', r.value, 'calibration_status', v_cal.status,
                                       'warn_ppm', (v_cal.params->>'warn_ppm')::numeric,
                                       'crit_ppm', (v_cal.params->>'crit_ppm')::numeric))
            on conflict (robot_id, type) where state in ('open', 'acknowledged')
            do update set severity = excluded.severity, last_seen_at = now(),
                          source_frame_id = excluded.source_frame_id, details = excluded.details;
            v_alerts := v_alerts || jsonb_build_object('type', v_type, 'severity', v_level);
        elsif v_est.ppm < 0.8 * (v_cal.params->>'warn_ppm')::double precision then
            -- histeresis: se cierra la alerta solo cuando baja bien por debajo del umbral
            update public.alerts set state = 'resolved', resolved_at = now()
             where robot_id = v_robot and type = v_type and state in ('open', 'acknowledged');
        end if;
    end loop;

    return jsonb_build_object('status', 'stored', 'frame_id', v_event,
                              'stream_version', v_stream, 'readings', v_n_reads,
                              'alerts', v_alerts);

exception when others then
    return jsonb_build_object('status', 'rejected', 'error', sqlerrm, 'sqlstate', sqlstate);
end;
$$;

revoke all on function public.ingest_telemetry(jsonb) from public, anon, authenticated;
grant execute on function public.ingest_telemetry(jsonb) to service_role;

revoke all on function public.calibrate_r0_clean_air(uuid, text, int, int, text) from public, anon, authenticated;
grant execute on function public.calibrate_r0_clean_air(uuid, text, int, int, text) to service_role;

-- =====================================================================
-- CENTINELA - 04_vistas.sql
-- Vistas para WEB y MOVIL. Ambas apps consultan EXACTAMENTE lo mismo, asi
-- nunca habra "80 ppm" en la web y "950 ppm" en la app.
-- security_invoker = true -> respeta RLS del usuario que consulta.
-- =====================================================================

-- Todas las lecturas ya interpretadas
create or replace view public.v_readings_interpreted
with (security_invoker = true) as
select
    f.id              as frame_id,
    f.robot_id,
    f.stream_version,
    f.captured_at,
    f.received_at,
    c.channel_key,
    c.metric_type,
    sr.value          as raw_value,
    sr.boolean_value,
    sr.quality        as raw_quality,
    -- valor listo para mostrar: gas = ppm estimados (null si no hay calibracion)
    case when c.metric_type = 'gas_raw' then round(est.ppm::numeric, 1)::double precision
         else sr.value end                                   as display_value,
    case when c.metric_type = 'gas_raw' then 'ppm' else c.unit_code end as display_unit,
    -- senal relativa 0-100 (siempre disponible para gases, sirve de barra/tacometro)
    case when c.metric_type = 'gas_raw' then round(est.signal_pct::numeric, 1)::double precision end as signal_pct,
    case when c.metric_type = 'gas_raw' then public.gas_level(est.ppm, cal.params) end as level,
    case
        when sr.quality = 'not_installed' then 'not_installed'
        when c.metric_type = 'gas_raw' then
            case cal.status when 'lab_validated'   then 'high'
                            when 'field_clean_air' then 'medium'
                            else 'uncalibrated' end
        when sr.quality = 'valid' then 'high'
        else sr.quality
    end                                                       as confidence,
    case when c.metric_type = 'gas_raw' then cal.status end   as calibration_status,
    case when c.metric_type = 'gas_raw' then cal.gas_label end as gas_label
from public.sensor_readings sr
join public.telemetry_frames f on f.id = sr.frame_id
join public.channels c         on c.id = sr.channel_id
left join lateral public.calibration_for(f.robot_id, c.channel_key) cal
       on c.metric_type = 'gas_raw'
left join lateral public.mq_estimate(sr.value, cal.params) est
       on c.metric_type = 'gas_raw' and sr.value is not null and cal.params is not null;

comment on view public.v_readings_interpreted is
'confidence: high | medium (R0 en aire limpio, estimacion) | uncalibrated (mostrar solo signal_pct, NO ppm) | not_installed';

-- Ultima lectura de cada canal por robot (para el dashboard en vivo)
create or replace view public.v_latest_readings
with (security_invoker = true) as
select v.*
from public.v_readings_interpreted v
where v.frame_id in (
    select distinct on (robot_id) id
    from public.telemetry_frames
    order by robot_id, stream_version desc
);

-- Estado actual de cada robot (una fila por robot)
create or replace view public.v_robot_status
with (security_invoker = true) as
select
    rb.id                       as robot_id,
    rb.public_code,
    rb.home_id,
    lf.id                       as frame_id,
    lf.captured_at,
    lf.received_at,
    round(extract(epoch from (now() - lf.received_at))::numeric, 1) as seconds_since_last,
    case
        when lf.id is null then 'never'
        when now() - lf.received_at < interval '10 seconds' then 'online'
        when now() - lf.received_at < interval '60 seconds' then 'stale'
        else 'offline'
    end                         as connection,
    d.firmware_version,
    fp.x_m, fp.y_m, fp.theta_rad, fp.quality as pose_quality,
    rs.reported_state, rs.navigation_state, rs.motors_active, rs.wifi_rssi_dbm,
    br.voltage_v                as battery_voltage_v,
    br.soc_pct                  as battery_soc_pct,
    br.quality                  as battery_quality,
    coalesce(al.open_alerts, 0) as open_alerts,
    al.worst_severity
from public.robots rb
left join lateral (
    select * from public.telemetry_frames t
    where t.robot_id = rb.id order by t.stream_version desc limit 1
) lf on true
left join public.devices d         on d.robot_id = rb.id
left join public.frame_poses fp    on fp.frame_id = lf.id
left join public.robot_states rs   on rs.frame_id = lf.id
left join public.battery_readings br on br.frame_id = lf.id
left join lateral (
    select count(*) as open_alerts,
           case when bool_or(severity = 'critical') then 'critical'
                when bool_or(severity = 'warning')  then 'warning'
                else 'info' end as worst_severity
    from public.alerts a
    where a.robot_id = rb.id and a.state in ('open', 'acknowledged')
) al on true;

-- Alertas vivas con datos legibles
create or replace view public.v_active_alerts
with (security_invoker = true) as
select a.id, a.robot_id, rb.public_code, rb.home_id, a.type, a.severity, a.state, a.origin,
       a.opened_at, a.last_seen_at, a.details
from public.alerts a
join public.robots rb on rb.id = a.robot_id
where a.state in ('open', 'acknowledged');

grant select on public.v_readings_interpreted, public.v_latest_readings,
                public.v_robot_status, public.v_active_alerts to authenticated;

-- =====================================================================
-- CENTINELA - 05_rls.sql
-- Seguridad: lectura solo para miembros activos del domicilio.
-- Escritura de telemetria: SOLO via ingest_telemetry (service_role).
-- =====================================================================

-- Funciones auxiliares (security definer para evitar recursion de RLS)
create or replace function public.is_home_member(p_home_id uuid)
returns boolean language sql stable security definer set search_path = public as $$
    select exists (select 1 from public.memberships m
                   where m.home_id = p_home_id and m.user_id = auth.uid() and m.status = 'active');
$$;

create or replace function public.is_home_technician(p_home_id uuid)
returns boolean language sql stable security definer set search_path = public as $$
    select exists (select 1 from public.memberships m
                   where m.home_id = p_home_id and m.user_id = auth.uid()
                     and m.status = 'active' and m.role = 'technician');
$$;

create or replace function public.can_read_robot(p_robot_id uuid)
returns boolean language sql stable security definer set search_path = public as $$
    select exists (select 1 from public.robots r
                   where r.id = p_robot_id and public.is_home_member(r.home_id));
$$;

create or replace function public.can_read_frame(p_frame_id uuid)
returns boolean language sql stable security definer set search_path = public as $$
    select exists (select 1 from public.telemetry_frames f
                   where f.id = p_frame_id and public.can_read_robot(f.robot_id));
$$;

-- Los tecnicos reconocen alertas desde web/movil con: supabase.rpc('ack_alert', { p_alert_id })
create or replace function public.ack_alert(p_alert_id uuid)
returns boolean language plpgsql security definer set search_path = public as $$
declare v_home uuid;
begin
    select rb.home_id into v_home from public.alerts a
      join public.robots rb on rb.id = a.robot_id where a.id = p_alert_id;
    if v_home is null or not public.is_home_technician(v_home) then
        raise exception 'sin permiso para reconocer esta alerta';
    end if;
    update public.alerts set state = 'acknowledged'
     where id = p_alert_id and state = 'open';
    return found;
end;
$$;

grant execute on function public.is_home_member(uuid), public.is_home_technician(uuid),
      public.can_read_robot(uuid), public.can_read_frame(uuid), public.ack_alert(uuid)
      to authenticated;

-- RLS en todas las tablas
alter table public.homes             enable row level security;
alter table public.memberships       enable row level security;
alter table public.robots            enable row level security;
alter table public.devices           enable row level security;
alter table public.device_sessions   enable row level security;
alter table public.map_versions      enable row level security;
alter table public.missions          enable row level security;
alter table public.telemetry_frames  enable row level security;
alter table public.channel_catalog   enable row level security;
alter table public.channels          enable row level security;
alter table public.sensor_readings   enable row level security;
alter table public.frame_poses       enable row level security;
alter table public.robot_states      enable row level security;
alter table public.battery_readings  enable row level security;
alter table public.alerts            enable row level security;
alter table public.gas_calibrations  enable row level security;

-- Politicas de LECTURA (to authenticated)
drop policy if exists p_homes_read on public.homes;
create policy p_homes_read on public.homes for select to authenticated
    using (public.is_home_member(id));

drop policy if exists p_memberships_read on public.memberships;
create policy p_memberships_read on public.memberships for select to authenticated
    using (user_id = auth.uid());

drop policy if exists p_robots_read on public.robots;
create policy p_robots_read on public.robots for select to authenticated
    using (public.is_home_member(home_id));

drop policy if exists p_devices_read on public.devices;
create policy p_devices_read on public.devices for select to authenticated
    using (public.can_read_robot(robot_id));

drop policy if exists p_sessions_read on public.device_sessions;
create policy p_sessions_read on public.device_sessions for select to authenticated
    using (exists (select 1 from public.devices d
                   where d.id = device_id and public.can_read_robot(d.robot_id)));

drop policy if exists p_maps_read on public.map_versions;
create policy p_maps_read on public.map_versions for select to authenticated
    using (public.is_home_member(home_id));

drop policy if exists p_missions_read on public.missions;
create policy p_missions_read on public.missions for select to authenticated
    using (public.can_read_robot(robot_id));

drop policy if exists p_frames_read on public.telemetry_frames;
create policy p_frames_read on public.telemetry_frames for select to authenticated
    using (public.can_read_robot(robot_id));

drop policy if exists p_catalog_read on public.channel_catalog;
create policy p_catalog_read on public.channel_catalog for select to authenticated
    using (true);

drop policy if exists p_channels_read on public.channels;
create policy p_channels_read on public.channels for select to authenticated
    using (public.can_read_robot(robot_id));

drop policy if exists p_readings_read on public.sensor_readings;
create policy p_readings_read on public.sensor_readings for select to authenticated
    using (public.can_read_frame(frame_id));

drop policy if exists p_poses_read on public.frame_poses;
create policy p_poses_read on public.frame_poses for select to authenticated
    using (public.can_read_frame(frame_id));

drop policy if exists p_states_read on public.robot_states;
create policy p_states_read on public.robot_states for select to authenticated
    using (public.can_read_frame(frame_id));

drop policy if exists p_battery_read on public.battery_readings;
create policy p_battery_read on public.battery_readings for select to authenticated
    using (public.can_read_frame(frame_id));

drop policy if exists p_alerts_read on public.alerts;
create policy p_alerts_read on public.alerts for select to authenticated
    using (public.can_read_robot(robot_id));

drop policy if exists p_cal_read on public.gas_calibrations;
create policy p_cal_read on public.gas_calibrations for select to authenticated
    using (robot_id is null or public.can_read_robot(robot_id));
