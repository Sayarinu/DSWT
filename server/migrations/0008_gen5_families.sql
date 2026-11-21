-- Add the Gen V game families (protocol/wire.py, "Game variants"): family 4
-- Black/White (versions 20 White, 21 Black) and family 5 Black 2/White 2 (22
-- White 2, 23 Black 2), with the same language IDs as Gen IV.
CREATE OR REPLACE FUNCTION dswt.valid_variant(code bigint) RETURNS boolean
LANGUAGE sql IMMUTABLE SET search_path = pg_catalog AS $$
    SELECT code = 1 OR (code BETWEEN 0 AND 4294967295
        AND ((code >> 24)::integer, ((code >> 16) & 255)::integer)
            IN ((1, 10), (1, 11), (2, 12), (3, 7), (3, 8), (4, 20), (4, 21), (5, 22), (5, 23))
        AND ((code >> 8) & 255)::integer IN (1, 2, 3, 4, 5, 7, 8))
$$;

-- Destination bits follow the family: bit 3 Black/White, bit 4 Black 2/White 2.
-- Pokémon never cross generations, so an offer's set lies within its owner's
-- pool: bits 0-2 for a Gen IV owner, bits 3-4 for a Gen V owner. Every stored
-- set is Gen IV and belongs to a Gen IV owner.
ALTER TABLE dswt.offer_destinations DROP CONSTRAINT offer_destinations_destinations_check;
ALTER TABLE dswt.offer_destinations ADD CONSTRAINT offer_destinations_destinations_check
    CHECK (destinations BETWEEN 1 AND 31);
CREATE FUNCTION dswt.check_destination_pool() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM dswt.offers o JOIN dswt.identity_variants v USING (save_id)
            WHERE o.trade_id = NEW.trade_id AND (NEW.destinations & ~(CASE
                WHEN v.variant <> 1 AND v.variant >> 24 IN (4, 5) THEN 24 ELSE 7 END)) = 0) THEN
        RAISE EXCEPTION 'offer destinations are outside its owner''s generation' USING ERRCODE = '23514';
    END IF;
    RETURN NULL;
END $$;
CREATE TRIGGER destination_pool AFTER INSERT ON dswt.offer_destinations
    FOR EACH ROW EXECUTE FUNCTION dswt.check_destination_pool();
REVOKE ALL ON FUNCTION dswt.valid_variant(bigint), dswt.check_destination_pool() FROM PUBLIC;
