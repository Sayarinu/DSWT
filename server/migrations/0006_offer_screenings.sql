-- Record each offer's name screening: whether the validator refused its nickname
-- or original trainer's name under a numbered rule (rule 1: the Nintendo Switch
-- word list). The matcher pairs only offers that passed the current rule, and an
-- offer the rule refused goes back to its owner. A screening never changes.
-- Offers stored before this migration have none; the backend screens them.
CREATE TABLE dswt.offer_screenings (
    trade_id dswt.id NOT NULL REFERENCES dswt.offers,
    rule smallint NOT NULL CHECK (rule >= 1),
    refused boolean NOT NULL,
    PRIMARY KEY (trade_id, rule)
);
CREATE TRIGGER immutable_row BEFORE UPDATE OR DELETE ON dswt.offer_screenings
    FOR EACH ROW EXECUTE FUNCTION dswt.reject_change();
CREATE TRIGGER retain_table BEFORE TRUNCATE ON dswt.offer_screenings
    FOR EACH STATEMENT EXECUTE FUNCTION dswt.reject_change();
CREATE TRIGGER audit_rows AFTER INSERT ON dswt.offer_screenings
    FOR EACH ROW EXECUTE FUNCTION dswt.audit_row('trade_id', 'rule');

-- Every new offer commits together with a passing screening: Submit refuses names
-- before an offer exists.
CREATE FUNCTION dswt.check_offer_screening() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM dswt.offer_screenings WHERE trade_id = NEW.trade_id AND NOT refused) THEN
        RAISE EXCEPTION 'offer has no passing screening' USING ERRCODE = '23514';
    END IF;
    RETURN NULL;
END $$;
CREATE CONSTRAINT TRIGGER offer_screening AFTER INSERT ON dswt.offers
    DEFERRABLE INITIALLY DEFERRED FOR EACH ROW EXECUTE FUNCTION dswt.check_offer_screening();
REVOKE ALL ON dswt.offer_screenings FROM PUBLIC;
REVOKE ALL ON FUNCTION dswt.check_offer_screening() FROM PUBLIC;

-- A server return is a CANCELLED event without an operation: the owner did not
-- ask for it, so it uses none of their counters. Only a refused offer may have one.
DO $$
DECLARE constraint_name name;
BEGIN
    SELECT conname INTO STRICT constraint_name FROM pg_constraint
        WHERE conrelid = 'dswt.trade_events'::regclass AND contype = 'c'
            AND pg_get_constraintdef(oid) LIKE '%ARRAY[2, 4, 5, 6]%operation_id IS NOT NULL%';
    EXECUTE format('ALTER TABLE dswt.trade_events DROP CONSTRAINT %I', constraint_name);
END $$;
ALTER TABLE dswt.trade_events ADD CONSTRAINT trade_events_operation
    CHECK (kind = 4 OR (kind IN (2, 5, 6)) = (operation_id IS NOT NULL));

CREATE OR REPLACE FUNCTION dswt.validate_event(item dswt.trade_events) RETURNS void LANGUAGE plpgsql AS $$
DECLARE operation dswt.operations; valid boolean;
BEGIN
    IF item.operation_id IS NOT NULL THEN
        SELECT * INTO operation FROM dswt.operations
            WHERE save_id = item.save_id AND operation_id = item.operation_id;
        IF NOT FOUND OR operation.event_sequence <> item.sequence OR ((item.kind = 6) <> (operation.result <> 1))
            OR (item.kind <> 6 AND operation.opcode <> CASE item.kind WHEN 2 THEN 2 WHEN 4 THEN 4 ELSE 6 END) THEN
            RAISE EXCEPTION 'event operation is inconsistent' USING ERRCODE = '23514';
        END IF;
    ELSIF item.kind = 4 AND NOT EXISTS (SELECT 1 FROM dswt.offer_screenings
            WHERE trade_id = item.trade_id AND refused) THEN
        RAISE EXCEPTION 'server return has no refused screening' USING ERRCODE = '23514';
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

CREATE OR REPLACE FUNCTION dswt.verify_replay() RETURNS bigint LANGUAGE plpgsql
SET search_path = pg_catalog SET timezone = 'UTC' SET bytea_output = 'hex' AS $$
DECLARE
    item record;
    previous jsonb;
    projection_name text;
    different boolean;
    row_count bigint := 0;
    allowed_tables text[] := ARRAY['save_identities', 'payloads', 'offers', 'ownership',
        'matches', 'receipts', 'operations', 'trade_events', 'acknowledgements', 'matcher_attempts',
        'identity_variants', 'offer_destinations', 'offer_screenings'];
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
    IF EXISTS (SELECT 1 FROM dswt.save_identities i
        WHERE NOT EXISTS (SELECT 1 FROM dswt.identity_variants v WHERE v.save_id = i.save_id)) THEN
        RAISE EXCEPTION 'replay identity variant is missing' USING ERRCODE = '23514';
    END IF;
    IF EXISTS (SELECT 1 FROM dswt.offers o
        WHERE NOT EXISTS (SELECT 1 FROM dswt.offer_destinations d WHERE d.trade_id = o.trade_id)) THEN
        RAISE EXCEPTION 'replay offer destinations are missing' USING ERRCODE = '23514';
    END IF;
    PERFORM dswt.verify_foreign_keys();
    PERFORM dswt.validate_offer(trade_id) FROM dswt.offers;
    PERFORM dswt.validate_event(e) FROM dswt.trade_events e;
    PERFORM dswt.validate_match(m) FROM dswt.matches m;
    PERFORM dswt.validate_acknowledgement(a) FROM dswt.acknowledgements a;
    DROP TABLE pg_temp.wt_replay_rows;
    RETURN row_count;
END $$;
