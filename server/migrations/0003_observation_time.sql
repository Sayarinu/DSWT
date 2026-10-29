-- Observational wall time is not an ownership/recovery authority. Old ages stay unknown.
ALTER TABLE platinumwt.audit_journal ADD COLUMN recorded_at timestamptz;
ALTER TABLE platinumwt.audit_journal ALTER COLUMN recorded_at SET DEFAULT clock_timestamp();
CREATE INDEX offer_observation_time ON platinumwt.audit_journal
    ((row_key ->> 'trade_id'), ((after_image ->> 'last_event')::numeric))
    INCLUDE (recorded_at) WHERE table_name = 'offers';
