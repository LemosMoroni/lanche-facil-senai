-- Execute no Supabase: Dashboard > SQL Editor > New Query
-- Cole todo o conteúdo abaixo e clique em "Run"

CREATE TABLE IF NOT EXISTS alunos (
  id        BIGSERIAL PRIMARY KEY,
  nome      TEXT NOT NULL,
  turma     TEXT NOT NULL,
  ativo     BOOLEAN NOT NULL DEFAULT true,
  criado_em TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS cartoes (
  id        BIGSERIAL PRIMARY KEY,
  numero    TEXT NOT NULL UNIQUE,
  uid       TEXT NOT NULL UNIQUE,
  aluno_id  BIGINT REFERENCES alunos(id),
  ativo     BOOLEAN NOT NULL DEFAULT true,
  criado_em TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS registros (
  id          BIGSERIAL PRIMARY KEY,
  uid         TEXT,
  aluno_id    BIGINT,
  aluno_nome  TEXT,
  aluno_turma TEXT,
  status      TEXT,
  motivo      TEXT,
  origem      TEXT DEFAULT 'online',
  criado_em   TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS historico_vinculos (
  id        BIGSERIAL PRIMARY KEY,
  cartao_id BIGINT NOT NULL REFERENCES cartoes(id),
  aluno_id  BIGINT REFERENCES alunos(id),
  acao      TEXT NOT NULL,
  criado_em TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Desabilitar RLS (o servidor usa service_role key que já bypassa, mas por segurança deixar explícito)
ALTER TABLE alunos             DISABLE ROW LEVEL SECURITY;
ALTER TABLE cartoes            DISABLE ROW LEVEL SECURITY;
ALTER TABLE registros          DISABLE ROW LEVEL SECURITY;
ALTER TABLE historico_vinculos DISABLE ROW LEVEL SECURITY;
