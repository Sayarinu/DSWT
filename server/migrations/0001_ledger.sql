CREATE SCHEMA platinumwt;
REVOKE ALL ON SCHEMA platinumwt FROM PUBLIC;

CREATE DOMAIN platinumwt.id AS uuid
    CHECK (VALUE <> '00000000-0000-0000-0000-000000000000');
CREATE DOMAIN platinumwt.u64 AS numeric
    CHECK (VALUE BETWEEN 0 AND 18446744073709551615 AND VALUE = trunc(VALUE));
CREATE DOMAIN platinumwt.digest AS bytea CHECK (octet_length(VALUE) = 32);

CREATE TABLE platinumwt.save_identities (
    save_id platinumwt.id PRIMARY KEY,
    enrollment_id platinumwt.id NOT NULL UNIQUE,
    credential_verifier platinumwt.digest NOT NULL,
    next_counter platinumwt.u64 NOT NULL DEFAULT 1 CHECK (next_counter > 0),
    event_highwater platinumwt.u64 NOT NULL DEFAULT 0
);

CREATE TABLE platinumwt.payloads (
    asset_id platinumwt.id PRIMARY KEY,
    pk4 bytea NOT NULL CHECK (octet_length(pk4) = 136),
    sha256 platinumwt.digest NOT NULL CHECK (sha256 = pg_catalog.sha256(pk4)),
    UNIQUE (asset_id, sha256)
);

CREATE TABLE platinumwt.offers (
    trade_id platinumwt.id PRIMARY KEY,
    save_id platinumwt.id NOT NULL REFERENCES platinumwt.save_identities,
    offered_asset_id platinumwt.id NOT NULL UNIQUE REFERENCES platinumwt.payloads,
    state smallint NOT NULL CHECK (state BETWEEN 1 AND 4),
    receipt_id platinumwt.id UNIQUE,
    last_event platinumwt.u64 NOT NULL CHECK (last_event > 0),
    UNIQUE (trade_id, save_id),
    CHECK ((state = 1) = (receipt_id IS NULL))
);
CREATE UNIQUE INDEX one_pending_offer ON platinumwt.offers(save_id) WHERE state <> 4;

CREATE TABLE platinumwt.matches (
    match_id platinumwt.id PRIMARY KEY,
    operation_id platinumwt.id NOT NULL UNIQUE,
    first_trade_id platinumwt.id NOT NULL UNIQUE REFERENCES platinumwt.offers,
    second_trade_id platinumwt.id NOT NULL UNIQUE REFERENCES platinumwt.offers,
    CHECK (first_trade_id < second_trade_id)
);

CREATE TABLE platinumwt.receipts (
    receipt_id platinumwt.id PRIMARY KEY,
    trade_id platinumwt.id NOT NULL UNIQUE,
    save_id platinumwt.id NOT NULL,
    asset_id platinumwt.id NOT NULL UNIQUE REFERENCES platinumwt.payloads,
    delivery_kind smallint NOT NULL CHECK (delivery_kind IN (1, 2)),
    match_id platinumwt.id REFERENCES platinumwt.matches,
    UNIQUE (trade_id, save_id, receipt_id),
    UNIQUE (receipt_id, save_id),
    UNIQUE (receipt_id, save_id, trade_id, asset_id),
    FOREIGN KEY (trade_id, save_id) REFERENCES platinumwt.offers(trade_id, save_id),
    CHECK ((delivery_kind = 1) = (match_id IS NOT NULL))
);
ALTER TABLE platinumwt.offers ADD FOREIGN KEY (trade_id, save_id, receipt_id)
    REFERENCES platinumwt.receipts(trade_id, save_id, receipt_id) DEFERRABLE INITIALLY DEFERRED;

CREATE TABLE platinumwt.ownership (
    asset_id platinumwt.id PRIMARY KEY REFERENCES platinumwt.payloads,
    save_id platinumwt.id NOT NULL,
    trade_id platinumwt.id NOT NULL,
    location smallint NOT NULL CHECK (location BETWEEN 1 AND 3),
    last_event platinumwt.u64 NOT NULL CHECK (last_event > 0),
    FOREIGN KEY (trade_id, save_id) REFERENCES platinumwt.offers(trade_id, save_id)
);
-- Every immutable payload must retain exactly one authoritative location.
ALTER TABLE platinumwt.payloads ADD FOREIGN KEY (asset_id)
    REFERENCES platinumwt.ownership DEFERRABLE INITIALLY DEFERRED;

CREATE TABLE platinumwt.operations (
    save_id platinumwt.id NOT NULL REFERENCES platinumwt.save_identities,
    operation_id platinumwt.id NOT NULL,
    counter platinumwt.u64 NOT NULL CHECK (counter > 0 AND counter < 18446744073709551615),
    opcode smallint NOT NULL CHECK (opcode IN (2, 4, 6)),
    request_sha256 platinumwt.digest NOT NULL,
    result smallint NOT NULL CHECK (result IN (1, 3, 5, 6, 8)),
    event_sequence platinumwt.u64 NOT NULL CHECK (event_sequence > 0),
    outcome bytea NOT NULL,
    PRIMARY KEY (save_id, operation_id),
    UNIQUE (save_id, counter),
    CHECK ((result = 1 AND octet_length(outcome) = 146)
        OR (result <> 1 AND octet_length(outcome) = 0))
);

CREATE TABLE platinumwt.trade_events (
    save_id platinumwt.id NOT NULL REFERENCES platinumwt.save_identities,
    sequence platinumwt.u64 NOT NULL CHECK (sequence > 0),
    version smallint NOT NULL DEFAULT 1 CHECK (version = 1),
    kind smallint NOT NULL CHECK (kind BETWEEN 1 AND 6),
    trade_id platinumwt.id,
    operation_id platinumwt.id,
    asset_id platinumwt.id REFERENCES platinumwt.payloads,
    payload_sha256 platinumwt.digest,
    receipt_id platinumwt.id,
    state smallint NOT NULL CHECK (state BETWEEN 0 AND 4),
    PRIMARY KEY (save_id, sequence),
    UNIQUE (save_id, operation_id),
    UNIQUE (save_id, sequence, operation_id),
    UNIQUE (save_id, sequence, trade_id, state),
    FOREIGN KEY (trade_id, save_id) REFERENCES platinumwt.offers(trade_id, save_id),
    FOREIGN KEY (receipt_id, save_id) REFERENCES platinumwt.receipts(receipt_id, save_id),
    FOREIGN KEY (receipt_id, save_id, trade_id, asset_id)
        REFERENCES platinumwt.receipts(receipt_id, save_id, trade_id, asset_id),
    FOREIGN KEY (asset_id, payload_sha256) REFERENCES platinumwt.payloads(asset_id, sha256),
    FOREIGN KEY (save_id, operation_id) REFERENCES platinumwt.operations
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
ALTER TABLE platinumwt.operations ADD FOREIGN KEY (save_id, event_sequence, operation_id)
    REFERENCES platinumwt.trade_events(save_id, sequence, operation_id) DEFERRABLE INITIALLY DEFERRED;
ALTER TABLE platinumwt.offers ADD FOREIGN KEY (save_id, last_event, trade_id, state)
    REFERENCES platinumwt.trade_events(save_id, sequence, trade_id, state) DEFERRABLE INITIALLY DEFERRED;
ALTER TABLE platinumwt.ownership ADD FOREIGN KEY (save_id, last_event)
    REFERENCES platinumwt.trade_events(save_id, sequence) DEFERRABLE INITIALLY DEFERRED;

CREATE TABLE platinumwt.acknowledgements (
    receipt_id platinumwt.id PRIMARY KEY,
    save_id platinumwt.id NOT NULL,
    operation_id platinumwt.id NOT NULL,
    applied_sha256 platinumwt.digest NOT NULL,
    applied_generation platinumwt.u64 NOT NULL CHECK (applied_generation > 0),
    FOREIGN KEY (receipt_id, save_id) REFERENCES platinumwt.receipts(receipt_id, save_id),
    FOREIGN KEY (save_id, operation_id) REFERENCES platinumwt.operations
        DEFERRABLE INITIALLY DEFERRED
);

CREATE TABLE platinumwt.audit_journal (
    sequence bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    version smallint NOT NULL DEFAULT 1 CHECK (version = 1),
    table_name name NOT NULL,
    row_key jsonb NOT NULL,
    before_image jsonb,
    after_image jsonb NOT NULL
);

CREATE FUNCTION platinumwt.reject_change() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    RAISE EXCEPTION 'retained evidence cannot be changed' USING ERRCODE = '23514';
END $$;

CREATE FUNCTION platinumwt.fixed_columns(table_name name, image jsonb) RETURNS jsonb
LANGUAGE sql IMMUTABLE AS $$
    SELECT CASE table_name
        WHEN 'save_identities' THEN image - ARRAY['next_counter', 'event_highwater']
        WHEN 'offers' THEN image - ARRAY['state', 'receipt_id', 'last_event']
        WHEN 'ownership' THEN image - ARRAY['save_id', 'trade_id', 'location', 'last_event']
        ELSE image END
$$;

CREATE FUNCTION platinumwt.valid_initial(table_name name, image jsonb) RETURNS boolean
LANGUAGE sql IMMUTABLE AS $$
    SELECT CASE table_name
        WHEN 'save_identities' THEN (image ->> 'next_counter')::numeric = 1
            AND (image ->> 'event_highwater')::numeric = 0
        WHEN 'offers' THEN (image ->> 'state')::integer = 1 AND image ->> 'receipt_id' IS NULL
        WHEN 'ownership' THEN (image ->> 'location')::integer = 1
        ELSE true END
$$;

CREATE FUNCTION platinumwt.valid_transition(table_name name, before_image jsonb, after_image jsonb)
RETURNS boolean LANGUAGE sql IMMUTABLE AS $$
    SELECT platinumwt.fixed_columns(table_name, before_image) = platinumwt.fixed_columns(table_name, after_image)
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
            AND (after_image ->> 'last_event')::numeric > (before_image ->> 'last_event')::numeric)
        ELSE false END
$$;

CREATE FUNCTION platinumwt.guard_initial() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF platinumwt.valid_initial(TG_TABLE_NAME, to_jsonb(NEW)) IS NOT TRUE THEN
        RAISE EXCEPTION 'invalid initial state' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END $$;

CREATE FUNCTION platinumwt.guard_transition() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF platinumwt.valid_transition(TG_TABLE_NAME, to_jsonb(OLD), to_jsonb(NEW)) IS NOT TRUE THEN
        RAISE EXCEPTION 'invalid state transition' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END $$;

CREATE FUNCTION platinumwt.audit_row() RETURNS trigger LANGUAGE plpgsql
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
        INSERT INTO platinumwt.audit_journal(table_name, row_key, after_image)
            VALUES (TG_TABLE_NAME, key, image);
    ELSIF OLD IS DISTINCT FROM NEW THEN
        INSERT INTO platinumwt.audit_journal(table_name, row_key, before_image, after_image)
            VALUES (TG_TABLE_NAME, key, to_jsonb(OLD), image);
    END IF;
    RETURN NULL;
END $$;

CREATE FUNCTION platinumwt.advance_counter() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    UPDATE platinumwt.save_identities SET next_counter = next_counter + 1
        WHERE save_id = NEW.save_id AND next_counter = NEW.counter;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'mutation counter mismatch' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END $$;
CREATE TRIGGER operation_counter BEFORE INSERT ON platinumwt.operations
    FOR EACH ROW EXECUTE FUNCTION platinumwt.advance_counter();

CREATE FUNCTION platinumwt.advance_event() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    UPDATE platinumwt.save_identities SET event_highwater = NEW.sequence
        WHERE save_id = NEW.save_id AND event_highwater = NEW.sequence - 1;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'event sequence mismatch' USING ERRCODE = '23514';
    END IF;
    RETURN NEW;
END $$;
CREATE TRIGGER event_counter BEFORE INSERT ON platinumwt.trade_events
    FOR EACH ROW EXECUTE FUNCTION platinumwt.advance_event();

CREATE FUNCTION platinumwt.check_event() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE operation platinumwt.operations; valid boolean;
BEGIN
    IF NEW.operation_id IS NOT NULL THEN
        SELECT * INTO operation FROM platinumwt.operations
            WHERE save_id = NEW.save_id AND operation_id = NEW.operation_id;
        IF NOT FOUND OR operation.event_sequence <> NEW.sequence OR ((NEW.kind = 6) <> (operation.result <> 1))
            OR (NEW.kind <> 6 AND operation.opcode <> CASE NEW.kind WHEN 2 THEN 2 WHEN 4 THEN 4 ELSE 6 END) THEN
            RAISE EXCEPTION 'event operation is inconsistent' USING ERRCODE = '23514';
        END IF;
    END IF;
    IF NEW.kind = 2 THEN
        SELECT EXISTS (SELECT 1 FROM platinumwt.offers WHERE trade_id = NEW.trade_id
            AND save_id = NEW.save_id AND offered_asset_id = NEW.asset_id) INTO valid;
        IF NOT valid THEN
            RAISE EXCEPTION 'submission event has wrong asset' USING ERRCODE = '23514';
        END IF;
    ELSIF NEW.kind IN (3, 4) THEN
        SELECT delivery_kind = CASE NEW.kind WHEN 3 THEN 1 ELSE 2 END INTO valid
            FROM platinumwt.receipts WHERE receipt_id = NEW.receipt_id;
        IF valid IS DISTINCT FROM true THEN
            RAISE EXCEPTION 'event delivery kind is inconsistent' USING ERRCODE = '23514';
        END IF;
    END IF;
    RETURN NULL;
END $$;
CREATE CONSTRAINT TRIGGER event_content AFTER INSERT ON platinumwt.trade_events
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION platinumwt.check_event();

CREATE FUNCTION platinumwt.check_identity() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE identity_row platinumwt.save_identities; last_event numeric; last_counter numeric;
BEGIN
    SELECT * INTO identity_row FROM platinumwt.save_identities WHERE save_id = NEW.save_id;
    SELECT sequence INTO last_event FROM platinumwt.trade_events
        WHERE save_id = NEW.save_id ORDER BY sequence DESC LIMIT 1;
    SELECT counter INTO last_counter FROM platinumwt.operations
        WHERE save_id = NEW.save_id ORDER BY counter DESC LIMIT 1;
    IF last_event IS NULL OR identity_row.event_highwater <> last_event
        OR identity_row.next_counter <> coalesce(last_counter, 0) + 1 THEN
        RAISE EXCEPTION 'identity checkpoint is inconsistent' USING ERRCODE = '23514';
    END IF;
    RETURN NULL;
END $$;
CREATE CONSTRAINT TRIGGER identity_checkpoint AFTER INSERT OR UPDATE ON platinumwt.save_identities
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION platinumwt.check_identity();

CREATE FUNCTION platinumwt.validate_offer(target uuid) RETURNS void LANGUAGE plpgsql AS $$
DECLARE offer platinumwt.offers; valid boolean;
BEGIN
    SELECT * INTO offer FROM platinumwt.offers WHERE trade_id = target;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'missing offer' USING ERRCODE = '23514';
    END IF;
    IF offer.state = 1 THEN
        SELECT EXISTS (SELECT 1 FROM platinumwt.ownership o
            WHERE o.asset_id = offer.offered_asset_id AND o.save_id = offer.save_id
                AND o.trade_id = target AND o.location = 1) INTO valid;
    ELSE
        SELECT EXISTS (SELECT 1 FROM platinumwt.receipts r
            JOIN platinumwt.ownership o USING (asset_id, save_id, trade_id)
            WHERE r.receipt_id = offer.receipt_id AND r.trade_id = target
                AND r.save_id = offer.save_id AND o.location = CASE WHEN offer.state = 4 THEN 3 ELSE 2 END
                AND (offer.state = 4 OR r.delivery_kind = CASE WHEN offer.state = 2 THEN 1 ELSE 2 END)
                AND ((r.delivery_kind = 2 AND r.asset_id = offer.offered_asset_id)
                    OR (r.delivery_kind = 1 AND EXISTS (
                        SELECT 1 FROM platinumwt.matches m JOIN platinumwt.offers peer
                            ON peer.trade_id = CASE WHEN m.first_trade_id = target
                                THEN m.second_trade_id ELSE m.first_trade_id END
                        WHERE m.match_id = r.match_id AND target IN (m.first_trade_id, m.second_trade_id)
                            AND peer.save_id <> offer.save_id AND peer.state IN (2, 4)
                            AND peer.offered_asset_id = r.asset_id)))) INTO valid;
    END IF;
    IF NOT valid THEN
        RAISE EXCEPTION 'offer ownership is inconsistent' USING ERRCODE = '23514';
    END IF;
    IF offer.state = 4 AND NOT EXISTS (SELECT 1 FROM platinumwt.acknowledgements
        WHERE receipt_id = offer.receipt_id AND save_id = offer.save_id) THEN
        RAISE EXCEPTION 'acknowledged offer has no applied receipt' USING ERRCODE = '23514';
    END IF;
END $$;

CREATE FUNCTION platinumwt.check_offer() RETURNS trigger LANGUAGE plpgsql AS $$
DECLARE valid boolean;
BEGIN
    PERFORM platinumwt.validate_offer(NEW.trade_id);
    IF TG_TABLE_NAME = 'ownership' THEN
        SELECT EXISTS (SELECT 1 FROM platinumwt.ownership o JOIN platinumwt.offers t
            ON t.trade_id = o.trade_id AND t.save_id = o.save_id
            WHERE o.asset_id = NEW.asset_id AND ((o.location = 1 AND t.state = 1
                AND t.offered_asset_id = o.asset_id) OR (o.location IN (2, 3) AND EXISTS (
                    SELECT 1 FROM platinumwt.receipts r WHERE r.receipt_id = t.receipt_id
                        AND r.asset_id = o.asset_id)))) INTO valid;
        IF NOT valid THEN
            RAISE EXCEPTION 'asset location is inconsistent' USING ERRCODE = '23514';
        END IF;
    END IF;
    IF TG_OP = 'UPDATE' AND OLD.trade_id <> NEW.trade_id THEN
        PERFORM platinumwt.validate_offer(OLD.trade_id);
    END IF;
    RETURN NULL;
END $$;
CREATE CONSTRAINT TRIGGER offer_ownership AFTER INSERT OR UPDATE ON platinumwt.offers
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION platinumwt.check_offer();
CREATE CONSTRAINT TRIGGER asset_ownership AFTER INSERT OR UPDATE ON platinumwt.ownership
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION platinumwt.check_offer();
CREATE CONSTRAINT TRIGGER receipt_ownership AFTER INSERT ON platinumwt.receipts
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION platinumwt.check_offer();

CREATE FUNCTION platinumwt.check_match() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    PERFORM platinumwt.validate_offer(NEW.first_trade_id);
    PERFORM platinumwt.validate_offer(NEW.second_trade_id);
    IF NOT EXISTS (SELECT 1 FROM platinumwt.offers a JOIN platinumwt.offers b
        ON b.trade_id = NEW.second_trade_id
        JOIN platinumwt.receipts ar ON ar.receipt_id = a.receipt_id
        JOIN platinumwt.receipts br ON br.receipt_id = b.receipt_id
        WHERE a.trade_id = NEW.first_trade_id AND a.save_id <> b.save_id
            AND a.state IN (2, 4) AND b.state IN (2, 4)
            AND ar.match_id = NEW.match_id AND br.match_id = NEW.match_id) THEN
        RAISE EXCEPTION 'match must commit both distinct owners' USING ERRCODE = '23514';
    END IF;
    RETURN NULL;
END $$;
CREATE CONSTRAINT TRIGGER match_pair AFTER INSERT ON platinumwt.matches
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION platinumwt.check_match();

CREATE FUNCTION platinumwt.check_acknowledgement() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM platinumwt.receipts r
        JOIN platinumwt.offers t USING (trade_id, save_id)
        JOIN platinumwt.operations op ON op.save_id = r.save_id AND op.operation_id = NEW.operation_id
        WHERE r.receipt_id = NEW.receipt_id AND r.save_id = NEW.save_id AND t.state = 4
            AND t.last_event = op.event_sequence AND op.opcode = 6 AND op.result = 1) THEN
        RAISE EXCEPTION 'acknowledgement is inconsistent' USING ERRCODE = '23514';
    END IF;
    RETURN NULL;
END $$;
CREATE CONSTRAINT TRIGGER applied_receipt AFTER INSERT ON platinumwt.acknowledgements
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION platinumwt.check_acknowledgement();

CREATE TRIGGER identity_columns BEFORE UPDATE ON platinumwt.save_identities
    FOR EACH ROW EXECUTE FUNCTION platinumwt.guard_transition();
CREATE TRIGGER identity_initial BEFORE INSERT ON platinumwt.save_identities
    FOR EACH ROW EXECUTE FUNCTION platinumwt.guard_initial();
CREATE TRIGGER offer_initial BEFORE INSERT ON platinumwt.offers
    FOR EACH ROW EXECUTE FUNCTION platinumwt.guard_initial();
CREATE TRIGGER ownership_initial BEFORE INSERT ON platinumwt.ownership
    FOR EACH ROW EXECUTE FUNCTION platinumwt.guard_initial();
CREATE TRIGGER offer_columns BEFORE UPDATE ON platinumwt.offers
    FOR EACH ROW EXECUTE FUNCTION platinumwt.guard_transition();
CREATE TRIGGER ownership_columns BEFORE UPDATE ON platinumwt.ownership
    FOR EACH ROW EXECUTE FUNCTION platinumwt.guard_transition();

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
        EXECUTE format('CREATE TRIGGER retain_rows BEFORE DELETE ON platinumwt.%I FOR EACH ROW EXECUTE FUNCTION platinumwt.reject_change()', table_name);
        EXECUTE format('CREATE TRIGGER retain_table BEFORE TRUNCATE ON platinumwt.%I FOR EACH STATEMENT EXECUTE FUNCTION platinumwt.reject_change()', table_name);
        EXECUTE format('CREATE TRIGGER audit_rows AFTER INSERT OR UPDATE ON platinumwt.%I FOR EACH ROW EXECUTE FUNCTION platinumwt.audit_row(%s)', table_name, key_columns);
        IF table_name NOT IN ('save_identities', 'offers', 'ownership') THEN
            EXECUTE format('CREATE TRIGGER immutable_row BEFORE UPDATE ON platinumwt.%I FOR EACH ROW EXECUTE FUNCTION platinumwt.reject_change()', table_name);
        END IF;
    END LOOP;
END $$;
CREATE TRIGGER retain_audit BEFORE UPDATE OR DELETE ON platinumwt.audit_journal
    FOR EACH ROW EXECUTE FUNCTION platinumwt.reject_change();
CREATE TRIGGER retain_audit_table BEFORE TRUNCATE ON platinumwt.audit_journal
    FOR EACH STATEMENT EXECUTE FUNCTION platinumwt.reject_change();

-- Rebuild row images only in a private temporary table under a repeatable snapshot.
-- Never replay into live tables or print verifier, request or Pokemon bytes.
CREATE FUNCTION platinumwt.verify_replay() RETURNS bigint LANGUAGE plpgsql
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
    FOR item IN SELECT * FROM platinumwt.audit_journal ORDER BY sequence LOOP
        IF item.version <> 1 OR NOT item.table_name = ANY(allowed_tables) THEN
            RAISE EXCEPTION 'unsupported replay event' USING ERRCODE = '23514';
        END IF;
        SELECT row_data INTO previous FROM pg_temp.wt_replay_rows
            WHERE wt_replay_rows.table_name = item.table_name AND row_key = item.row_key;
        IF previous IS DISTINCT FROM item.before_image THEN
            RAISE EXCEPTION 'replay preimage mismatch' USING ERRCODE = '23514';
        END IF;
        IF (item.before_image IS NULL AND platinumwt.valid_initial(item.table_name, item.after_image) IS NOT TRUE)
            OR (item.before_image IS NOT NULL AND platinumwt.valid_transition(
                item.table_name, item.before_image, item.after_image) IS NOT TRUE) THEN
            RAISE EXCEPTION 'invalid replay transition' USING ERRCODE = '23514';
        END IF;
        INSERT INTO pg_temp.wt_replay_rows VALUES (item.table_name, item.row_key, item.after_image)
            ON CONFLICT (table_name, row_key) DO UPDATE SET row_data = EXCLUDED.row_data;
        row_count := row_count + 1;
    END LOOP;
    FOREACH projection_name IN ARRAY allowed_tables LOOP
        EXECUTE format('SELECT EXISTS ((SELECT to_jsonb(t) FROM platinumwt.%I t EXCEPT ALL SELECT row_data FROM pg_temp.wt_replay_rows WHERE table_name = $1) UNION ALL (SELECT row_data FROM pg_temp.wt_replay_rows WHERE table_name = $1 EXCEPT ALL SELECT to_jsonb(t) FROM platinumwt.%I t))', projection_name, projection_name)
            INTO different USING projection_name;
        IF different THEN
            RAISE EXCEPTION 'replay projection mismatch' USING ERRCODE = '23514';
        END IF;
    END LOOP;
    IF EXISTS (SELECT 1 FROM platinumwt.save_identities i
        LEFT JOIN (SELECT save_id, count(*) AS events FROM platinumwt.trade_events GROUP BY save_id) e USING (save_id)
        LEFT JOIN (SELECT save_id, count(*) AS operations FROM platinumwt.operations GROUP BY save_id) o USING (save_id)
        WHERE i.event_highwater <> coalesce(e.events, 0) OR i.next_counter <> coalesce(o.operations, 0) + 1) THEN
        RAISE EXCEPTION 'replay checkpoint history is incomplete' USING ERRCODE = '23514';
    END IF;
    PERFORM platinumwt.validate_offer(trade_id) FROM platinumwt.offers;
    DROP TABLE pg_temp.wt_replay_rows;
    RETURN row_count;
END $$;

REVOKE ALL ON ALL TABLES IN SCHEMA platinumwt FROM PUBLIC;
REVOKE ALL ON ALL FUNCTIONS IN SCHEMA platinumwt FROM PUBLIC;
