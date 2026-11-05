-- Stock-engine research probe, not a checkpoint implementation.
local function report(name, ok)
  assert(ok, name)
  texio.write_nl('term and log', 'STEMTEX_PROBE_PASS: ' .. name)
end

local hidden = 0
local original_stop_run = callback.find('stop_run')
local baseline = { value = 0 }
local n = node.new('kern')
n.kern = 0
node.setproperty(n, { value = 0 })

stemtex_probe = {}
function stemtex_probe.mutate()
  baseline.value = 1
  hidden = 1
  n.kern = 65536
  node.getproperty(n).value = 1
  callback.register('stop_run', function() end)
  stemtex_probe_global = 1
end

function stemtex_probe.check()
  report('tex-local-count-restored', tex.count[100] == 7)
  report('lua-global-survives-tex-group', stemtex_probe_global == 1)
  report('lua-table-survives-tex-group', baseline.value == 1)
  report('lua-upvalue-survives-tex-group', hidden == 1)
  report('lua-node-mutation-survives-tex-group', n.kern == 65536)
  report('lua-node-property-survives-tex-group', node.getproperty(n).value == 1)
  report('callback-registration-survives-tex-group', callback.find('stop_run') ~= original_stop_run)
  callback.register('stop_run', original_stop_run)

  local ok = pcall(function()
    hidden = 2
    baseline.value = 2
    error('intentional caught Lua error')
  end)
  report('pcall-catches-error-but-does-not-rollback', not ok and hidden == 2 and baseline.value == 2)

  local restored = assert(load(string.dump(function() return hidden end)))
  report('bytecode-is-not-closure-state-snapshot', restored() ~= hidden)

  node.setproperty(n, nil)
  node.free(n)
  stemtex_probe_global = nil
  texio.write_nl('term and log', 'STEMTEX_PROBE_COMPLETE: 9 checks; no pages shipped')
end
