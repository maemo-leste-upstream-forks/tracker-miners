python = find_program('python3')

env = environment()

# Needed to run the examples from the source tree, instead of
# relying on host-installed Tracker Miners.
env.prepend('LD_LIBRARY_PATH', tracker_sparql_uninstalled_dir)
env.set('TEST_ONTOLOGIES_DIR', tracker_uninstalled_nepomuk_ontologies_dir)

# Tell endpoint example to quit.
env.set('TRACKER_EXAMPLES_AUTOMATED_TEST', '1')

sandbox_python_args = [run_uninstalled, '--store-tmpdir']

examples = [
  'query-async',
  'query-sync',
]

foreach example_name: examples
  file = meson.current_source_dir() / '@0@.py'.format(example_name)
  test(example_name, python,
    args: sandbox_python_args + [file],
    env: env,
    suite: 'examples')
endforeach
