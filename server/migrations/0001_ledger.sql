CREATE SCHEMA dswt;
REVOKE ALL ON SCHEMA dswt FROM PUBLIC;

CREATE DOMAIN dswt.id AS uuid
    CHECK (VALUE <> '00000000-0000-0000-0000-000000000000');
CREATE DOMAIN dswt.u64 AS numeric
    CHECK (VALUE BETWEEN 0 AND 18446744073709551615 AND VALUE = trunc(VALUE));
CREATE DOMAIN dswt.digest AS bytea CHECK (octet_length(VALUE) = 32);

CREATE TABLE dswt.save_identities (
    save_id dswt.id PRIMARY KEY,
    enrollment_id dswt.id NOT NULL UNIQUE,
    credential_verifier dswt.digest NOT NULL,
    next_counter dswt.u64 NOT NULL DEFAULT 1 CHECK (next_counter > 0),
    event_highwater dswt.u64 NOT NULL DEFAULT 0
);

CREATE TABLE dswt.payloads (
    asset_id dswt.id PRIMARY KEY,
    pk4 bytea NOT NULL CHECK (octet_length(pk4) = 136),
    sha256 dswt.digest NOT NULL CHECK (sha256 = pg_catalog.sha256(pk4)),
    UNIQUE (asset_id, sha256)
);

CREATE TABLE dswt.offers (
    trade_id dswt.id PRIMARY KEY,
    save_id dswt.id NOT NULL REFERENCES dswt.save_identities,
    offered_asset_id dswt.id NOT NULL UNIQUE REFERENCES dswt.payloads,
    state smallint NOT NULL CHECK (state BETWEEN 1 AND 4),
    receipt_id dswt.id UNIQUE,
    last_event dswt.u64 NOT NULL CHECK (last_event > 0),
    event_kind smallint GENERATED ALWAYS AS ((state + 1)::smallint) STORED,
    UNIQUE (trade_id, save_id),
    UNIQUE (trade_id, save_id, receipt_id),
    CHECK ((state = 1) = (receipt_id IS NULL))
);
CREATE UNIQUE INDEX one_pending_offer ON dswt.offers(save_id) WHERE state <> 4;

CREATE TABLE dswt.matches (
    match_id dswt.id PRIMARY KEY,
    operation_id dswt.id NOT NULL UNIQUE,
    first_trade_id dswt.id NOT NULL UNIQUE REFERENCES dswt.offers,
    second_trade_id dswt.id NOT NULL UNIQUE REFERENCES dswt.offers,
    CHECK (first_trade_id < second_trade_id)
);

CREATE TABLE dswt.receipts (
    receipt_id dswt.id PRIMARY KEY,
    trade_id dswt.id NOT NULL UNIQUE,
    save_id dswt.id NOT NULL,
    asset_id dswt.id NOT NULL UNIQUE REFERENCES dswt.payloads,
    delivery_kind smallint NOT NULL CHECK (delivery_kind IN (1, 2)),
    match_id dswt.id REFERENCES dswt.matches,
    UNIQUE (trade_id, save_id, receipt_id),
    UNIQUE (receipt_id, save_id),
    UNIQUE (receipt_id, save_id, trade_id, asset_id),
    FOREIGN KEY (trade_id, save_id) REFERENCES dswt.offers(trade_id, save_id),
    CHECK ((delivery_kind = 1) = (match_id IS NOT NULL))
);
ALTER TABLE dswt.offers ADD FOREIGN KEY (trade_id, save_id, receipt_id)
    REFERENCES dswt.receipts(trade_id, save_id, receipt_id) DEFERRABLE INITIALLY DEFERRED;
ALTER TABLE dswt.receipts ADD FOREIGN KEY (trade_id, save_id, receipt_id)
    REFERENCES dswt.offers(trade_id, save_id, receipt_id) DEFERRABLE INITIALLY DEFERRED;

CREATE TABLE dswt.ownership (
    asset_id dswt.id PRIMARY KEY REFERENCES dswt.payloads,
    save_id dswt.id NOT NULL,
    trade_id dswt.id NOT NULL,
    location smallint NOT NULL CHECK (location BETWEEN 1 AND 3),
    last_event dswt.u64 NOT NULL CHECK (last_event > 0),
    FOREIGN KEY (trade_id, save_id) REFERENCES dswt.offers(trade_id, save_id)
);
-- Every immutable payload must retain exactly one authoritative location.
ALTER TABLE dswt.payloads ADD FOREIGN KEY (asset_id)
    REFERENCES dswt.ownership DEFERRABLE INITIALLY DEFERRED;

CREATE TABLE dswt.operations (
    save_id dswt.id NOT NULL REFERENCES dswt.save_identities,
    operation_id dswt.id NOT NULL,
    counter dswt.u64 NOT NULL CHECK (counter > 0 AND counter < 18446744073709551615),
    opcode smallint NOT NULL CHECK (opcode IN (2, 4, 6)),
    request_sha256 dswt.digest NOT NULL,
    result smallint NOT NULL CHECK (result IN (1, 3, 5, 6, 8)),
    event_sequence dswt.u64 NOT NULL CHECK (event_sequence > 0),
    outcome bytea NOT NULL,
    PRIMARY KEY (save_id, operation_id),
    UNIQUE (save_id, counter),
    CHECK ((result = 1 AND octet_length(outcome) = 146)
        OR (result <> 1 AND octet_length(outcome) = 0))
);

CREATE TABLE dswt.trade_events (
    save_id dswt.id NOT NULL REFERENCES dswt.save_identities,
    sequence dswt.u64 NOT NULL CHECK (sequence > 0),
    version smallint NOT NULL DEFAULT 1 CHECK (version = 1),
    kind smallint NOT NULL CHECK (kind BETWEEN 1 AND 6),
    trade_id dswt.id,
    operation_id dswt.id,
    asset_id dswt.id REFERENCES dswt.payloads,
    payload_sha256 dswt.digest,
    receipt_id dswt.id,
    state smallint NOT NULL CHECK (state BETWEEN 0 AND 4),
    PRIMARY KEY (save_id, sequence),
    UNIQUE (save_id, operation_id),
    UNIQUE (save_id, sequence, operation_id),
    UNIQUE (save_id, sequence, trade_id, state, kind),
    FOREIGN KEY (trade_id, save_id) REFERENCES dswt.offers(trade_id, save_id),
    FOREIGN KEY (receipt_id, save_id) REFERENCES dswt.receipts(receipt_id, save_id),
    FOREIGN KEY (receipt_id, save_id, trade_id, asset_id)
        REFERENCES dswt.receipts(receipt_id, save_id, trade_id, asset_id),
    FOREIGN KEY (asset_id, payload_sha256) REFERENCES dswt.payloads(asset_id, sha256),
    FOREIGN KEY (save_id, operation_id) REFERENCES dswt.operations
        DEFERRABLE INITIALLY DEFERRED,
    CHECK ((kind IN (2, 4, 5, 6)) = (operation_id IS NOT NULL)),
    CHECK (asset_id IS NULL OR payload_sha256 IS NOT NULL),
    CHECK (kind NOT IN (2, 3, 4, 5) OR asset_id IS NOT NULL),
    CHECK ((kind IN (3, 4, 5)) = (receipt_id IS NOT NULL)),
    CHECK (kind <> 1 OR (sequence = 1 AND asset_id IS NULL AND payload_sha256 IS NULL)),
    CHECK ((sequence = 1) = (kind = 1)),
    CHECK (kind = 6 OR ((kind = 1) = (trade_id IS NULL))),
    CHECK ((kind = 1 AND state = 0) OR (kind = 2 AND state = 1)
        OR (kind = 3 AND state = 2) OR (kind = 4 AND state = 3)
        OR (kind = 5 AND state = 4) OR kind = 6)
);
ALTER TABLE dswt.operations ADD FOREIGN KEY (save_id, event_sequence, operation_id)
    REFERENCES dswt.trade_events(save_id, sequence, operation_id) DEFERRABLE INITIALLY DEFERRED;
ALTER TABLE dswt.offers ADD FOREIGN KEY (save_id, last_event, trade_id, state, event_kind)
    REFERENCES dswt.trade_events(save_id, sequence, trade_id, state, kind) DEFERRABLE INITIALLY DEFERRED;
ALTER TABLE dswt.ownership ADD FOREIGN KEY (save_id, last_event)
    REFERENCES dswt.trade_events(save_id, sequence) DEFERRABLE INITIALLY DEFERRED;

CREATE TABLE dswt.acknowledgements (
    receipt_id dswt.id PRIMARY KEY,
    save_id dswt.id NOT NULL,
    operation_id dswt.id NOT NULL,
    applied_sha256 dswt.digest NOT NULL,
    applied_generation dswt.u64 NOT NULL CHECK (applied_generation > 0),
    FOREIGN KEY (receipt_id, save_id) REFERENCES dswt.receipts(receipt_id, save_id),
    FOREIGN KEY (save_id, operation_id) REFERENCES dswt.operations
        DEFERRABLE INITIALLY DEFERRED
);

CREATE TABLE dswt.audit_journal (
    sequence bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    version smallint NOT NULL DEFAULT 1 CHECK (version = 1),
    table_name name NOT NULL,
    row_key jsonb NOT NULL,
    before_image jsonb,
    after_image jsonb NOT NULL
);

CREATE FUNCTION dswt.reject_change() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    RAISE EXCEPTION 'retained evidence cannot be changed' USING ERRCODE = '23514';
END $$;

CREATE FUNCTION dswt.fixed_columns(table_name name, image jsonb) RETURNS jsonb
LANGUAGE sql IMMUTABLE AS $$
    SELECT CASE table_name
        WHEN 'save_identities' THEN image - ARRAY['next_counter', 'event_highwater']
        WHEN 'offers' THEN image - ARRAY['state', 'receipt_id', 'last_event', 'event_kind']
        WHEN 'ownership' THEN image - ARRAY['save_id', 'trade_id', 'location', 'last_event']
        ELSE image END
$$;

CREATE FUNCTION dswt.valid_initial(table_name name, image jsonb) RETURNS boolean
LANGUAGE sql IMMUTABLE AS $$
    SELECT CASE table_name
        WHEN 'save_identities' THEN (image ->> 'next_counter')::numeric = 1
            AND (image ->> 'event_highwater')::numeric = 0
        WHEN 'offers' THEN (image ->> 'state')::integer = 1 AND image ->> 'receipt_id' IS NULL
        WHEN 'ownership' THEN (image ->> 'location')::integer = 1
        ELSE true END
$$;

CREATE FUNCTION dswt.valid_transition(table_name name, before_image jsonb, after_image jsonb)
RETURNS boolean LANGUAGE sql IMMUTABLE AS $$
    SELECT dswt.fixed_columns(table_name, before_image) = dswt.fixed_columns(table_name, after_image)
        AND CASE table_name
        WHEN 'save_identities' THEN before_image = after_image OR (
            (after_image ->> 'next_counter')::numeric = (before_image ->> 'next_counter')::numeric + 1
                AND after_image ->> 'event_highwater' = before_image ->> 'event_highwater') OR (
            after_image ->> 'next_counter' = before_image ->> 'next_counter'
                AND (after_image ->> 'event_highwater')::numeric = (before_image ->> 'event_highwater')::numeric + 1)
        WHEN 'offers' THEN before_image = after_image OR (
            ((before_image ->> 'state')::integer = 1 AND (after_image ->> 'state')::integer IN (2, 3)
                OR (before_image ->> 'state')::integer IN (2, 3) AND (after_image ->> 'state')::integer = 4)
            AND (after_image ->> 'last_event')::numeric > (before_image ->> 'last_event')::numeric
            AND (before_image ->> 'receipt_id' IS NULL OR after_image ->> 'receipt_id' = before_image ->> 'receipt_id'))
        WHEN 'ownership' THEN before_image = after_image OR (
            ((before_image ->> 'location')::integer = 1 AND (after_image ->> 'location')::integer = 2
                OR (before_image ->> 'location')::integer = 2 AND (after_image ->> 'location')::integer = 3
                    AND after_image ->> 'save_id' = before_image ->> 'save_id'
                    AND after_image ->> 'trade_id' = before_image ->> 'trade_id')
            AND (after_image ->> 'save_id' <> before_image ->> 'save_id'
                OR (after_image ->> 'last_event')::numeric > (before_image ->> 'last_event')::numeric))
        ELSE false END
$$;

CREATE FUNCTION dswt.guard_initial() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF dswt.valid_initial(TG_TABLE_NAME, to_jsonb(NEW)) IS NOT TRUE THEN
        RAISE EXCEPTION 'invalid initial state' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END $$;

CREATE FUNCTION dswt.guard_transition() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF dswt.valid_transition(TG_TABLE_NAME, to_jsonb(OLD), to_jsonb(NEW)) IS NOT TRUE THEN
        RAISE EXCEPTION 'invalid state transition' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END $$;

CREATE FUNCTION dswt.audit_row() RETURNS trigger LANGUAGE plpgsql
SECURITY DEFINER SET search_path = pg_catalog SET timezone = 'UTC' SET bytea_output = 'hex' AS $$
DECLARE
    image jsonb := to_jsonb(NEW);
    key jsonb := '{}';
    column_name text;
BEGIN
    FOREACH column_name IN ARRAY TG_ARGV LOOP
        key := key || jsonb_build_object(column_name, image -> column_name);
    END LOOP;
    IF TG_OP = 'INSERT' THEN
        INSERT INTO dswt.audit_journal(table_name, row_key, after_image)
            VALUES (TG_TABLE_NAME, key, image);
    ELSIF OLD IS DISTINCT FROM NEW THEN
        INSERT INTO dswt.audit_journal(table_name, row_key, before_image, after_image)
            VALUES (TG_TABLE_NAME, key, to_jsonb(OLD), image);
    END IF;
    RETURN NULL;
END $$;

CREATE FUNCTION dswt.advance_counter() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    UPDATE dswt.save_identities SET next_counter = next_counter + 1
        WHERE save_id = NEW.save_id AND next_counter = NEW.counter;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'mutation counter mismatch' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END $$;
CREATE TRIGGER operation_counter BEFORE INSERT ON dswt.operations
    FOR EACH ROW EXECUTE FUNCTION dswt.advance_counter();

CREATE FUNCTION dswt.advance_event() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    UPDATE dswt.save_identities SET event_highwater = NEW.sequence
        WHERE save_id = NEW.save_id AND event_highwater = NEW.sequence - 1;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'event sequence mismatch' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END $$;
CREATE TRIGGER event_counter BEFORE INSERT ON dswt.trade_events
    FOR EACH ROW EXECUTE FUNCTION dswt.advance_event();

CREATE FUNCTION dswt.validate_event(item dswt.trade_events) RETURNS void LANGUAGE plpgsql AS $$
DECLARE operation dswt.operations; valid boolean;
BEGIN
    IF item.operation_id IS NOT NULL THEN
        SELECT * INTO operation FROM dswt.operations
            WHERE save_id = item.save_id AND operation_id = item.operation_id;
        IF NOT FOUND OR operation.event_sequence <> item.sequence OR ((item.kind = 6) <> (operation.result <> 1))
            OR (item.kind <> 6 AND operation.opcode <> CASE item.kind WHEN 2 THEN 2 WHEN 4 THEN 4 ELSE 6 END) THEN
            RAISE EXCEPTION 'event operation is inconsistent' USING ERRCODE = '23514';
        END IF;
    END IF;
    IF item.kind = 2 THEN
        SELECT EXISTS (SELECT 1 FROM dswt.offers WHERE trade_id = item.trade_id
            AND save_id = item.save_id AND offered_asset_id = item.asset_id) INTO valid;
        IF NOT valid THEN
            RAISE EXCEPTION 'submission event has wrong asset' USING ERRCODE = '23514';
        END IF;
    ELSIF item.kind IN (3, 4) THEN
        SELECT delivery_kind = CASE item.kind WHEN 3 THEN 1 ELSE 2 END INTO valid
            FROM dswt.receipts WHERE receipt_id = item.receipt_id;
        IF valid IS DISTINCT FROM true THEN
            RAISE EXCEPTION 'event delivery kind is inconsistent' USING ERRCODE = '23514';
        END IF;
    END IF;
    RETURN;
END $$;
CREATE FUNCTION dswt.check_event() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    PERFORM dswt.validate_event(NEW);
    RETURN NULL;
END $$;
CREATE CONSTRAINT TRIGGER event_content AFTER INSERT ON dswt.trade_events
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION dswt.check_event();

CREATE FUNCTION dswt.check_identity() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE identity_row dswt.save_identities; last_event numeric; last_counter numeric;
BEGIN
    SELECT * INTO identity_row FROM dswt.save_identities WHERE save_id = NEW.save_id;
    SELECT sequence INTO last_event FROM dswt.trade_events
        WHERE save_id = NEW.save_id ORDER BY sequence DESC LIMIT 1;
    SELECT counter INTO last_counter FROM dswt.operations
        WHERE save_id = NEW.save_id ORDER BY counter DESC LIMIT 1;
    IF last_event IS NULL OR identity_row.event_highwater <> last_event
        OR identity_row.next_counter <> coalesce(last_counter, 0) + 1 THEN
        RAISE EXCEPTION 'identity checkpoint is inconsistent' USING ERRCODE = '23514';
    END IF;
    RETURN NULL;
END $$;
CREATE CONSTRAINT TRIGGER identity_checkpoint AFTER INSERT OR UPDATE ON dswt.save_identities
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION dswt.check_identity();

CREATE FUNCTION dswt.validate_offer(target uuid) RETURNS void LANGUAGE plpgsql AS $$
DECLARE offer dswt.offers; valid boolean;
BEGIN
    SELECT * INTO offer FROM dswt.offers WHERE trade_id = target;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'missing offer' USING ERRCODE = '23514';
    END IF;
    IF offer.state = 1 THEN
        SELECT EXISTS (SELECT 1 FROM dswt.ownership o
            WHERE o.asset_id = offer.offered_asset_id AND o.save_id = offer.save_id
                AND o.trade_id = target AND o.location = 1 AND o.last_event = offer.last_event) INTO valid;
    ELSE
        SELECT EXISTS (SELECT 1 FROM dswt.receipts r
            JOIN dswt.ownership o USING (asset_id, save_id, trade_id)
            WHERE r.receipt_id = offer.receipt_id AND r.trade_id = target
                AND r.save_id = offer.save_id AND o.last_event = offer.last_event
                AND o.location = CASE WHEN offer.state = 4 THEN 3 ELSE 2 END
                AND (offer.state = 4 OR r.delivery_kind = CASE WHEN offer.state = 2 THEN 1 ELSE 2 END)
                AND ((r.delivery_kind = 2 AND r.asset_id = offer.offered_asset_id)
                    OR (r.delivery_kind = 1 AND EXISTS (
                        SELECT 1 FROM dswt.matches m JOIN dswt.offers peer
                            ON peer.trade_id = CASE WHEN m.first_trade_id = target
                                THEN m.second_trade_id ELSE m.first_trade_id END
                        WHERE m.match_id = r.match_id AND target IN (m.first_trade_id, m.second_trade_id)
                            AND peer.save_id <> offer.save_id AND peer.state IN (2, 4)
                            AND peer.offered_asset_id = r.asset_id)))) INTO valid;
    END IF;
    IF NOT valid THEN
        RAISE EXCEPTION 'offer ownership is inconsistent' USING ERRCODE = '23514';
    END IF;
    IF offer.state = 4 AND NOT EXISTS (SELECT 1 FROM dswt.acknowledgements
        WHERE receipt_id = offer.receipt_id AND save_id = offer.save_id) THEN
        RAISE EXCEPTION 'acknowledged offer has no applied receipt' USING ERRCODE = '23514';
    END IF;
END $$;

CREATE FUNCTION dswt.check_offer() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE valid boolean;
BEGIN
    PERFORM dswt.validate_offer(NEW.trade_id);
    IF TG_TABLE_NAME = 'ownership' THEN
        SELECT EXISTS (SELECT 1 FROM dswt.ownership o JOIN dswt.offers t
            ON t.trade_id = o.trade_id AND t.save_id = o.save_id
            WHERE o.asset_id = NEW.asset_id AND ((o.location = 1 AND t.state = 1
                AND t.offered_asset_id = o.asset_id) OR (o.location IN (2, 3) AND EXISTS (
                    SELECT 1 FROM dswt.receipts r WHERE r.receipt_id = t.receipt_id
                        AND r.asset_id = o.asset_id)))) INTO valid;
        IF NOT valid THEN
            RAISE EXCEPTION 'asset location is inconsistent' USING ERRCODE = '23514';
        END IF;
    END IF;
    IF TG_OP = 'UPDATE' AND OLD.trade_id <> NEW.trade_id THEN
        PERFORM dswt.validate_offer(OLD.trade_id);
    END IF;
    RETURN NULL;
END $$;
CREATE CONSTRAINT TRIGGER offer_ownership AFTER INSERT OR UPDATE ON dswt.offers
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION dswt.check_offer();
CREATE CONSTRAINT TRIGGER asset_ownership AFTER INSERT OR UPDATE ON dswt.ownership
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION dswt.check_offer();
CREATE CONSTRAINT TRIGGER receipt_ownership AFTER INSERT ON dswt.receipts
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION dswt.check_offer();

CREATE FUNCTION dswt.validate_match(item dswt.matches) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
    PERFORM dswt.validate_offer(item.first_trade_id);
    PERFORM dswt.validate_offer(item.second_trade_id);
    IF NOT EXISTS (SELECT 1 FROM dswt.offers a JOIN dswt.offers b
        ON b.trade_id = item.second_trade_id
        JOIN dswt.receipts ar ON ar.receipt_id = a.receipt_id
        JOIN dswt.receipts br ON br.receipt_id = b.receipt_id
        WHERE a.trade_id = item.first_trade_id AND a.save_id <> b.save_id
            AND a.state IN (2, 4) AND b.state IN (2, 4)
            AND ar.match_id = item.match_id AND br.match_id = item.match_id) THEN
        RAISE EXCEPTION 'match must commit both distinct owners' USING ERRCODE = '23514';
    END IF;
    RETURN;
END $$;
CREATE FUNCTION dswt.check_match() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    PERFORM dswt.validate_match(NEW);
    RETURN NULL;
END $$;
CREATE CONSTRAINT TRIGGER match_pair AFTER INSERT ON dswt.matches
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION dswt.check_match();

CREATE FUNCTION dswt.validate_acknowledgement(item dswt.acknowledgements) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM dswt.receipts r
        JOIN dswt.offers t USING (trade_id, save_id)
        JOIN dswt.operations op ON op.save_id = r.save_id AND op.operation_id = item.operation_id
        WHERE r.receipt_id = item.receipt_id AND r.save_id = item.save_id AND t.state = 4
            AND t.last_event = op.event_sequence AND op.opcode = 6 AND op.result = 1) THEN
        RAISE EXCEPTION 'acknowledgement is inconsistent' USING ERRCODE = '23514';
    END IF;
    RETURN;
END $$;
CREATE FUNCTION dswt.check_acknowledgement() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    PERFORM dswt.validate_acknowledgement(NEW);
    RETURN NULL;
END $$;
CREATE CONSTRAINT TRIGGER applied_receipt AFTER INSERT ON dswt.acknowledgements
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION dswt.check_acknowledgement();

CREATE TRIGGER identity_columns BEFORE UPDATE ON dswt.save_identities
    FOR EACH ROW EXECUTE FUNCTION dswt.guard_transition();
CREATE TRIGGER identity_initial BEFORE INSERT ON dswt.save_identities
    FOR EACH ROW EXECUTE FUNCTION dswt.guard_initial();
CREATE TRIGGER offer_initial BEFORE INSERT ON dswt.offers
    FOR EACH ROW EXECUTE FUNCTION dswt.guard_initial();
CREATE TRIGGER ownership_initial BEFORE INSERT ON dswt.ownership
    FOR EACH ROW EXECUTE FUNCTION dswt.guard_initial();
CREATE TRIGGER offer_columns BEFORE UPDATE ON dswt.offers
    FOR EACH ROW EXECUTE FUNCTION dswt.guard_transition();
CREATE TRIGGER ownership_columns BEFORE UPDATE ON dswt.ownership
    FOR EACH ROW EXECUTE FUNCTION dswt.guard_transition();

-- Audit every row change in the same transaction; forbid deletion and truncation.
DO $$
DECLARE table_name text; key_columns text;
BEGIN
    FOR table_name, key_columns IN SELECT * FROM (VALUES
        ('save_identities', '''save_id'''), ('payloads', '''asset_id'''),
        ('offers', '''trade_id'''), ('ownership', '''asset_id'''),
        ('matches', '''match_id'''), ('receipts', '''receipt_id'''),
        ('operations', '''save_id'', ''operation_id'''),
        ('trade_events', '''save_id'', ''sequence'''),
        ('acknowledgements', '''receipt_id''')) AS tables(name, keys)
    LOOP
        EXECUTE format('CREATE TRIGGER retain_rows BEFORE DELETE ON dswt.%I FOR EACH ROW EXECUTE FUNCTION dswt.reject_change()', table_name);
        EXECUTE format('CREATE TRIGGER retain_table BEFORE TRUNCATE ON dswt.%I FOR EACH STATEMENT EXECUTE FUNCTION dswt.reject_change()', table_name);
        EXECUTE format('CREATE TRIGGER audit_rows AFTER INSERT OR UPDATE ON dswt.%I FOR EACH ROW EXECUTE FUNCTION dswt.audit_row(%s)', table_name, key_columns);
        IF table_name NOT IN ('save_identities', 'offers', 'ownership') THEN
            EXECUTE format('CREATE TRIGGER immutable_row BEFORE UPDATE ON dswt.%I FOR EACH ROW EXECUTE FUNCTION dswt.reject_change()', table_name);
        END IF;
    END LOOP;
END $$;
CREATE TRIGGER retain_audit BEFORE UPDATE OR DELETE ON dswt.audit_journal
    FOR EACH ROW EXECUTE FUNCTION dswt.reject_change();
CREATE TRIGGER retain_audit_table BEFORE TRUNCATE ON dswt.audit_journal
    FOR EACH STATEMENT EXECUTE FUNCTION dswt.reject_change();

-- Recheck declared FKs even if a privileged restore bypassed their write triggers.
CREATE FUNCTION dswt.verify_foreign_keys() RETURNS void LANGUAGE plpgsql
SET search_path = pg_catalog AS $$
DECLARE constraint_row record; orphan boolean;
BEGIN
    FOR constraint_row IN
        SELECT child.relname AS child_name, parent.relname AS parent_name, pn.nspname AS parent_schema,
            string_agg(format('c.%I = p.%I', ca.attname, pa.attname), ' AND ' ORDER BY keys.ordinality) AS joins,
            string_agg(format('c.%I IS NOT NULL', ca.attname), ' AND ' ORDER BY keys.ordinality) AS present
        FROM pg_constraint fk JOIN pg_class child ON child.oid = fk.conrelid
        JOIN pg_namespace cn ON cn.oid = child.relnamespace
        JOIN pg_class parent ON parent.oid = fk.confrelid
        JOIN pg_namespace pn ON pn.oid = parent.relnamespace
        CROSS JOIN LATERAL unnest(fk.conkey, fk.confkey) WITH ORDINALITY AS keys(child_key, parent_key, ordinality)
        JOIN pg_attribute ca ON ca.attrelid = child.oid AND ca.attnum = keys.child_key
        JOIN pg_attribute pa ON pa.attrelid = parent.oid AND pa.attnum = keys.parent_key
        WHERE cn.nspname = 'dswt' AND fk.contype = 'f'
        GROUP BY fk.oid, child.relname, parent.relname, pn.nspname
    LOOP
        EXECUTE format('SELECT EXISTS (SELECT 1 FROM dswt.%I c WHERE %s AND NOT EXISTS (SELECT 1 FROM %I.%I p WHERE %s))',
            constraint_row.child_name, constraint_row.present, constraint_row.parent_schema,
            constraint_row.parent_name, constraint_row.joins) INTO orphan;
        IF orphan THEN
            RAISE EXCEPTION 'foreign key invariant failed' USING ERRCODE = '23514';
        END IF;
    END LOOP;
END $$;

-- Rebuild row images only in a private temporary table under a repeatable snapshot.
-- Never replay into live tables or print verifier, request or Pokemon bytes.
CREATE FUNCTION dswt.verify_replay() RETURNS bigint LANGUAGE plpgsql
SET search_path = pg_catalog SET timezone = 'UTC' SET bytea_output = 'hex' AS $$
DECLARE
    item record;
    previous jsonb;
    projection_name text;
    different boolean;
    row_count bigint := 0;
    allowed_tables text[] := ARRAY['save_identities', 'payloads', 'offers', 'ownership',
        'matches', 'receipts', 'operations', 'trade_events', 'acknowledgements'];
BEGIN
    CREATE TEMP TABLE wt_replay_rows (
        table_name name, row_key jsonb, row_data jsonb, PRIMARY KEY (table_name, row_key)
    ) ON COMMIT DROP;
    FOR item IN SELECT * FROM dswt.audit_journal ORDER BY sequence LOOP
        IF item.version <> 1 OR NOT item.table_name = ANY(allowed_tables) THEN
            RAISE EXCEPTION 'unsupported replay event' USING ERRCODE = '23514';
        END IF;
        SELECT row_data INTO previous FROM pg_temp.wt_replay_rows
            WHERE wt_replay_rows.table_name = item.table_name AND row_key = item.row_key;
        IF previous IS DISTINCT FROM item.before_image THEN
            RAISE EXCEPTION 'replay preimage mismatch' USING ERRCODE = '23514';
        END IF;
        IF (item.before_image IS NULL AND dswt.valid_initial(item.table_name, item.after_image) IS NOT TRUE)
            OR (item.before_image IS NOT NULL AND dswt.valid_transition(
                item.table_name, item.before_image, item.after_image) IS NOT TRUE) THEN
            RAISE EXCEPTION 'invalid replay transition' USING ERRCODE = '23514';
        END IF;
        INSERT INTO pg_temp.wt_replay_rows VALUES (item.table_name, item.row_key, item.after_image)
            ON CONFLICT (table_name, row_key) DO UPDATE SET row_data = EXCLUDED.row_data;
        row_count := row_count + 1;
    END LOOP;
    FOREACH projection_name IN ARRAY allowed_tables LOOP
        EXECUTE format('SELECT EXISTS ((SELECT to_jsonb(t) FROM dswt.%I t EXCEPT ALL SELECT row_data FROM pg_temp.wt_replay_rows WHERE table_name = $1) UNION ALL (SELECT row_data FROM pg_temp.wt_replay_rows WHERE table_name = $1 EXCEPT ALL SELECT to_jsonb(t) FROM dswt.%I t))', projection_name, projection_name)
            INTO different USING projection_name;
        IF different THEN
            RAISE EXCEPTION 'replay projection mismatch' USING ERRCODE = '23514';
        END IF;
    END LOOP;
    IF EXISTS (SELECT 1 FROM dswt.save_identities i
        LEFT JOIN (SELECT save_id, count(*) AS events, max(sequence) AS last_event FROM dswt.trade_events GROUP BY save_id) e USING (save_id)
        LEFT JOIN (SELECT save_id, count(*) AS operations, max(counter) AS last_counter FROM dswt.operations GROUP BY save_id) o USING (save_id)
        WHERE i.event_highwater = 0 OR i.event_highwater <> coalesce(e.events, 0)
            OR i.event_highwater <> coalesce(e.last_event, 0)
            OR i.next_counter <> coalesce(o.operations, 0) + 1
            OR i.next_counter <> coalesce(o.last_counter, 0) + 1) THEN
        RAISE EXCEPTION 'replay checkpoint history is incomplete' USING ERRCODE = '23514';
    END IF;
    PERFORM dswt.verify_foreign_keys();
    PERFORM dswt.validate_offer(trade_id) FROM dswt.offers;
    PERFORM dswt.validate_event(e) FROM dswt.trade_events e;
    PERFORM dswt.validate_match(m) FROM dswt.matches m;
    PERFORM dswt.validate_acknowledgement(a) FROM dswt.acknowledgements a;
    DROP TABLE pg_temp.wt_replay_rows;
    RETURN row_count;
END $$;

REVOKE ALL ON ALL TABLES IN SCHEMA dswt FROM PUBLIC;
REVOKE ALL ON ALL FUNCTIONS IN SCHEMA dswt FROM PUBLIC;
