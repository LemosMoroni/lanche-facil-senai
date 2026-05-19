require('dotenv').config();
const express  = require('express');
const { createClient } = require('@supabase/supabase-js');
const cors     = require('cors');
const path     = require('path');

const app  = express();
const PORT = process.env.PORT || 3000;

const supabase = createClient(process.env.SUPABASE_URL, process.env.SUPABASE_SERVICE_KEY);

app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

// ─── Cache em memória ─────────────────────────────────────────────────────────
const cacheUIDs = new Map();
const cacheHoje = new Set();

async function sincronizarCache() {
  try {
    const { data: cartoes } = await supabase
      .from('cartoes')
      .select('uid, aluno_id, alunos(nome, turma, ativo)')
      .eq('ativo', true)
      .not('aluno_id', 'is', null);

    cacheUIDs.clear();
    for (const c of cartoes || []) {
      if (c.alunos?.ativo) {
        cacheUIDs.set(c.uid, { aluno_id: c.aluno_id, aluno_nome: c.alunos.nome, aluno_turma: c.alunos.turma });
      }
    }

    const { data: retiradas } = await supabase
      .from('registros')
      .select('aluno_id')
      .eq('status', 'liberado')
      .gte('criado_em', inicioDoDia());

    cacheHoje.clear();
    for (const r of retiradas || []) if (r.aluno_id) cacheHoje.add(r.aluno_id);

    console.log(`[cache] ${cacheUIDs.size} UIDs | ${cacheHoje.size} já retiraram hoje`);
  } catch (err) {
    console.error('[cache] Erro:', err.message);
  }
}

function inicioDoDia() {
  const d = new Date();
  d.setHours(0, 0, 0, 0);
  return d.toISOString();
}

function agendarViradaDoDia() {
  const meia = new Date();
  meia.setHours(24, 0, 5, 0);
  setTimeout(() => {
    cacheHoje.clear();
    console.log('[cache] Meia-noite — retiradas zeradas.');
    agendarViradaDoDia();
  }, meia - Date.now());
}

// ─── Último cartão desconhecido ───────────────────────────────────────────────
let ultimoDesconhecido = { uid: null, at: 0 };

// ─── Segurança: device key ────────────────────────────────────────────────────
const DEVICE_KEY = process.env.DEVICE_KEY;

app.use('/verificar', (req, res, next) => {
  if (!DEVICE_KEY || req.headers['x-device-key'] !== DEVICE_KEY) {
    return res.status(401).json({ acesso: false, motivo: 'Não autorizado.' });
  }
  next();
});

// ─── POST /verificar — ESP8266 ────────────────────────────────────────────────
app.post('/verificar', async (req, res) => {
  const { uid, origem = 'online' } = req.body;
  if (!uid) return res.status(400).json({ acesso: false, motivo: 'UID não informado.' });

  const uidUpper = uid.trim().toUpperCase();
  const entrada  = cacheUIDs.get(uidUpper);

  if (!entrada) {
    ultimoDesconhecido = { uid: uidUpper, at: Date.now() };
    await gravarRegistro({ uid: uidUpper, status: 'negado', motivo: 'Cartão não cadastrado', origem });
    return res.json({ acesso: false, motivo: 'Cartão não cadastrado.' });
  }

  if (cacheHoje.has(entrada.aluno_id)) {
    await gravarRegistro({ uid: uidUpper, aluno_id: entrada.aluno_id, aluno_nome: entrada.aluno_nome, aluno_turma: entrada.aluno_turma, status: 'negado', motivo: 'Lanche já retirado hoje', origem });
    return res.json({ acesso: false, motivo: 'Lanche já retirado hoje.' });
  }

  cacheHoje.add(entrada.aluno_id);
  await gravarRegistro({ uid: uidUpper, aluno_id: entrada.aluno_id, aluno_nome: entrada.aluno_nome, aluno_turma: entrada.aluno_turma, status: 'liberado', origem });
  return res.json({ acesso: true, aluno: entrada.aluno_nome, turma: entrada.aluno_turma });
});

async function gravarRegistro(dados) {
  try {
    await supabase.from('registros').insert({
      uid:         dados.uid         ?? null,
      aluno_id:    dados.aluno_id    ?? null,
      aluno_nome:  dados.aluno_nome  ?? null,
      aluno_turma: dados.aluno_turma ?? null,
      status:      dados.status,
      motivo:      dados.motivo      ?? null,
      origem:      dados.origem      ?? 'online',
    });
  } catch (err) {
    console.error('[registros] Erro:', err.message);
  }
}

// ─── POST /sync-offline ───────────────────────────────────────────────────────
app.post('/sync-offline', async (req, res) => {
  if (!DEVICE_KEY || req.headers['x-device-key'] !== DEVICE_KEY) {
    return res.status(401).json({ erro: 'Não autorizado.' });
  }
  const { registros } = req.body;
  if (!Array.isArray(registros) || !registros.length) return res.json({ sincronizados: 0 });

  const rows = registros.map(r => ({
    uid:         String(r.uid).toUpperCase(),
    aluno_id:    r.aluno_id    || null,
    aluno_nome:  r.aluno_nome  || null,
    aluno_turma: r.aluno_turma || null,
    status:      ['liberado','negado'].includes(r.status) ? r.status : 'negado',
    motivo:      r.motivo      || null,
    origem:      'offline',
    criado_em:   r.criado_em   || new Date().toISOString(),
  }));

  const { error } = await supabase.from('registros').insert(rows);
  if (error) {
    console.error('[sync-offline] Erro:', error.message);
    return res.status(500).json({ erro: error.message });
  }

  for (const r of registros) if (r.status === 'liberado' && r.aluno_id) cacheHoje.add(r.aluno_id);
  res.json({ sincronizados: registros.length });
});

// ─── GET /sync-cache — NodeMCU baixa UIDs para modo offline ──────────────────
app.get('/sync-cache', (req, res) => {
  if (!DEVICE_KEY || req.headers['x-device-key'] !== DEVICE_KEY) {
    return res.status(401).json({ erro: 'Não autorizado.' });
  }
  const uids = [];
  for (const [uid, d] of cacheUIDs) {
    uids.push({ uid, aluno_id: d.aluno_id, nome: d.aluno_nome, turma: d.aluno_turma });
  }
  res.json({ uids });
});

// ─── Último cartão desconhecido ───────────────────────────────────────────────
app.get('/api/ultimo-desconhecido', (req, res) => {
  const valido = ultimoDesconhecido.uid && (Date.now() - ultimoDesconhecido.at < 30000);
  res.json(valido ? { uid: ultimoDesconhecido.uid } : { uid: null });
});

app.delete('/api/ultimo-desconhecido', (req, res) => {
  ultimoDesconhecido = { uid: null, at: 0 };
  res.json({ ok: true });
});

// ─── Alunos ───────────────────────────────────────────────────────────────────
app.get('/api/alunos', async (req, res) => {
  const { data, error } = await supabase
    .from('alunos').select('*').eq('ativo', true).order('nome');
  if (error) return res.status(500).json({ erro: error.message });
  res.json(data);
});

app.post('/api/alunos', async (req, res) => {
  const { nome, turma } = req.body;
  if (!nome || !turma) return res.status(400).json({ erro: 'Nome e turma são obrigatórios.' });
  const { data, error } = await supabase
    .from('alunos').insert({ nome: nome.trim(), turma: turma.trim() }).select().single();
  if (error) return res.status(500).json({ erro: error.message });
  res.status(201).json(data);
});

app.put('/api/alunos/:id', async (req, res) => {
  const { nome, turma } = req.body;
  if (!nome || !turma) return res.status(400).json({ erro: 'Nome e turma são obrigatórios.' });
  const { data, error } = await supabase
    .from('alunos').update({ nome: nome.trim(), turma: turma.trim() })
    .eq('id', req.params.id).eq('ativo', true).select().single();
  if (error?.code === 'PGRST116') return res.status(404).json({ erro: 'Aluno não encontrado.' });
  if (error) return res.status(500).json({ erro: error.message });
  await sincronizarCache();
  res.json(data);
});

app.delete('/api/alunos/:id', async (req, res) => {
  const { data, error } = await supabase
    .from('alunos').update({ ativo: false })
    .eq('id', req.params.id).eq('ativo', true).select().single();
  if (error?.code === 'PGRST116') return res.status(404).json({ erro: 'Aluno não encontrado.' });
  if (error) return res.status(500).json({ erro: error.message });
  await sincronizarCache();
  res.json({ mensagem: 'Aluno removido com sucesso.' });
});

// ─── Cartões ──────────────────────────────────────────────────────────────────
app.get('/api/cartoes', async (req, res) => {
  const { data: cartoes, error } = await supabase
    .from('cartoes')
    .select('id, numero, uid, aluno_id, ativo, criado_em, alunos(nome, turma)')
    .eq('ativo', true)
    .order('criado_em', { ascending: false });
  if (error) return res.status(500).json({ erro: error.message });

  const ids = cartoes.map(c => c.id);
  let histRows = [];
  if (ids.length) {
    const { data } = await supabase
      .from('historico_vinculos')
      .select('id, cartao_id, acao, criado_em, alunos(nome)')
      .in('cartao_id', ids)
      .order('criado_em', { ascending: false });
    histRows = data || [];
  }

  const histMap = {};
  for (const h of histRows) {
    (histMap[h.cartao_id] ??= []).push({ id: h.id, acao: h.acao, criado_em: h.criado_em, aluno_nome: h.alunos?.nome ?? null });
  }

  res.json(cartoes.map(c => ({
    id: c.id, numero: c.numero, uid: c.uid, aluno_id: c.aluno_id, ativo: c.ativo, criado_em: c.criado_em,
    aluno_nome:  c.alunos?.nome  ?? null,
    aluno_turma: c.alunos?.turma ?? null,
    historico:   histMap[c.id]   ?? [],
  })));
});

app.post('/api/cartoes', async (req, res) => {
  const { numero, uid } = req.body;
  if (!numero || !uid) return res.status(400).json({ erro: 'Número e UID são obrigatórios.' });
  const { data, error } = await supabase
    .from('cartoes').insert({ numero: numero.trim(), uid: uid.trim().toUpperCase() }).select().single();
  if (error?.code === '23505') return res.status(409).json({ erro: 'Número ou UID já cadastrado.' });
  if (error) return res.status(500).json({ erro: error.message });
  res.status(201).json({ ...data, aluno_nome: null, aluno_turma: null, historico: [] });
});

app.put('/api/cartoes/:id/vincular', async (req, res) => {
  const { aluno_id } = req.body;
  if (!aluno_id) return res.status(400).json({ erro: 'aluno_id é obrigatório.' });

  const { data: aluno } = await supabase.from('alunos').select('id').eq('id', aluno_id).eq('ativo', true).single();
  if (!aluno) return res.status(404).json({ erro: 'Aluno não encontrado.' });

  const { data, error } = await supabase
    .from('cartoes').update({ aluno_id }).eq('id', req.params.id).eq('ativo', true).select().single();
  if (error?.code === 'PGRST116') return res.status(404).json({ erro: 'Cartão não encontrado.' });
  if (error) return res.status(500).json({ erro: error.message });

  await supabase.from('historico_vinculos').insert({ cartao_id: Number(req.params.id), aluno_id, acao: 'vinculado' });
  await sincronizarCache();
  res.json({ mensagem: 'Cartão vinculado com sucesso.' });
});

app.put('/api/cartoes/:id/desvincular', async (req, res) => {
  const { data: cartao } = await supabase.from('cartoes').select('aluno_id').eq('id', req.params.id).eq('ativo', true).single();
  if (!cartao) return res.status(404).json({ erro: 'Cartão não encontrado.' });
  if (!cartao.aluno_id) return res.status(400).json({ erro: 'Cartão não está vinculado.' });

  await supabase.from('cartoes').update({ aluno_id: null }).eq('id', req.params.id);
  await supabase.from('historico_vinculos').insert({ cartao_id: Number(req.params.id), aluno_id: cartao.aluno_id, acao: 'desvinculado' });
  await sincronizarCache();
  res.json({ mensagem: 'Cartão desvinculado com sucesso.' });
});

// ─── Relatórios ───────────────────────────────────────────────────────────────
app.get('/api/relatorio/hoje', async (req, res) => {
  const { data, error } = await supabase
    .from('registros').select('*').gte('criado_em', inicioDoDia()).order('criado_em', { ascending: false });
  if (error) return res.status(500).json({ erro: error.message });
  res.json(data);
});

app.get('/api/relatorio/contadores', async (req, res) => {
  const inicio = inicioDoDia();
  const [a, lib, neg, cart] = await Promise.all([
    supabase.from('alunos').select('*',    { count: 'exact', head: true }).eq('ativo', true),
    supabase.from('registros').select('*', { count: 'exact', head: true }).eq('status', 'liberado').gte('criado_em', inicio),
    supabase.from('registros').select('*', { count: 'exact', head: true }).eq('status', 'negado').gte('criado_em', inicio),
    supabase.from('cartoes').select('*',   { count: 'exact', head: true }).eq('ativo', true),
  ]);
  res.json({ total_alunos: a.count, retirados: lib.count, negados: neg.count, total_cartoes: cart.count });
});

app.post('/api/relatorio/reset', async (req, res) => {
  const { data, error } = await supabase
    .from('registros').delete().gte('criado_em', inicioDoDia()).select();
  if (error) return res.status(500).json({ erro: error.message });
  cacheHoje.clear();
  res.json({ mensagem: `${data?.length ?? 0} registro(s) removido(s).` });
});

// ─── Inicialização ────────────────────────────────────────────────────────────
sincronizarCache().then(() => {
  setInterval(sincronizarCache, 5 * 60 * 1000);
  agendarViradaDoDia();
  app.listen(PORT, () => console.log(`Servidor rodando na porta ${PORT}`));
});
