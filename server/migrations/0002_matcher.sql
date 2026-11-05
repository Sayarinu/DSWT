-- Retain worker outcomes, including a definitive empty/single-entry poll.
-- Leave the checksummed v1 migration intact; extend its replay allowlist below.
ALTER TABLE dswt.matches ADD UNIQUE (match_id, operation_id);
CREATE TABLE dswt.matcher_attempts (
    operation_id dswt.id PRIMARY KEY,
    match_id dswt.id UNIQUE,
    UNIQUE (operation_id, match_id),
    FOREIGN KEY (match_id, operation_id) REFERENCES dswt.matches(match_id, operation_id)
);
CREATE TRIGGER immutable_row BEFORE UPDATE OR DELETE ON dswt.matcher_attempts
    FOR EACH ROW EXECUTE FUNCTION dswt.reject_change();
CREATE TRIGGER retain_table BEFORE TRUNCATE ON dswt.matcher_attempts
    FOR EACH STATEMENT EXECUTE FUNCTION dswt.reject_change();
CREATE TRIGGER audit_rows AFTER INSERT ON dswt.matcher_attempts
    FOR EACH ROW EXECUTE FUNCTION dswt.audit_row('operation_id');
INSERT INTO dswt.matcher_attempts SELECT operation_id, match_id FROM dswt.matches;
ALTER TABLE dswt.matches ADD FOREIGN KEY (operation_id, match_id)
    REFERENCES dswt.matcher_attempts(operation_id, match_id) DEFERRABLE INITIALLY DEFERRED;
REVOKE ALL ON dswt.matcher_attempts FROM PUBLIC;

CREATE INDEX queued_offer_candidates ON dswt.offers(trade_id)
    INCLUDE (save_id, offered_asset_id, last_event) WHERE state = 1;
CREATE UNIQUE INDEX offer_admission_order ON dswt.audit_journal ((row_key ->> 'trade_id'))
    INCLUDE (sequence) WHERE table_name = 'offers' AND before_image IS NULL;

CREATE OR REPLACE FUNCTION dswt.verify_replay() RETURNS bigint LANGUAGE plpgsql
SET search_path = pg_catalog SET timezone = 'UTC' SET bytea_output = 'hex' AS $$
DECLARE
    item record;
    previous jsonb;
    projection_name text;
    different boolean;
    row_count bigint := 0;
    allowed_tables text[] := ARRAY['save_identities', 'payloads', 'offers', 'ownership',
        'matches', 'receipts', 'operations', 'trade_events', 'acknowledgements', 'matcher_attempts'];
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
