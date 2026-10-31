import importlib.util
import json
from types import SimpleNamespace
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch


root = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('batch_demo',root/'notebooks/seekdb-hipvs-batch-demo/lib/seekdb_demo.py')
demo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(demo)


class BatchDemoTest(unittest.TestCase):
    def test_missing_probe_has_actionable_error(self):
        with patch.object(demo,'sql_rows',return_value=[]):
            with self.assertRaisesRegex(RuntimeError,'section 3'):
                demo.single_query('t_cuvs')
            with self.assertRaisesRegex(RuntimeError,'section 3'):
                demo.wait_index_ready(floor=0,timeout=0)

    def test_index_timeout_is_not_ready(self):
        with patch.object(demo,'_probe_vector',return_value='[0,0,0]'):
            with self.assertRaisesRegex(TimeoutError,'did not return stable'):
                demo.wait_index_ready(floor=0,timeout=0)

    def test_startup_cell_imports_path_without_previous_cell(self):
        notebook=json.loads((root/'notebooks/seekdb-hipvs-batch-demo/seekdb_hipvs_batch_demo.ipynb').read_text(encoding='utf-8'))
        startup=''.join(notebook['cells'][7]['source'])
        # A failed server start must be reported as such, never NameError(Path).
        with self.assertRaisesRegex(RuntimeError,'observer failed to start'):
            exec(startup,{'sk':SimpleNamespace(start_observer=lambda base:False)})

    def test_out_of_order_result_cells_explain_prerequisites(self):
        notebook=json.loads((root/'notebooks/seekdb-hipvs-batch-demo/seekdb_hipvs_batch_demo.ipynb').read_text(encoding='utf-8'))
        for index in (10,12,13,15,17,19):
            with self.subTest(cell=index):
                with self.assertRaisesRegex(RuntimeError,'prerequisites missing'):
                    exec(''.join(notebook['cells'][index]['source']),{'sk':demo})

    def test_failed_sweep_does_not_keep_old_results(self):
        notebook=json.loads((root/'notebooks/seekdb-hipvs-batch-demo/seekdb_hipvs_batch_demo.ipynb').read_text(encoding='utf-8'))
        def failed_sweep(*args):
            raise RuntimeError('sweep failed')
        state={'sk':SimpleNamespace(require_notebook_state=demo.require_notebook_state,sweep=failed_sweep),
               '_DATA_READY':True,'PROBE_SIZES':(100,),'df':'old result','score':'old score'}
        with self.assertRaisesRegex(RuntimeError,'sweep failed'):
            exec(''.join(notebook['cells'][12]['source']),state)
        self.assertNotIn('df',state)
        self.assertNotIn('score',state)

    def test_failed_data_setup_invalidates_previous_run(self):
        notebook=json.loads((root/'notebooks/seekdb-hipvs-batch-demo/seekdb_hipvs_batch_demo.ipynb').read_text(encoding='utf-8'))
        state={'sk':SimpleNamespace(setup_data=lambda *args:(False,'load failed')),
               '_DATA_READY':True,'df':'old result','score':'old score'}
        with self.assertRaisesRegex(AssertionError,'data load failed'):
            exec(''.join(notebook['cells'][8]['source']),state)
        self.assertNotIn('_DATA_READY',state)
        self.assertNotIn('df',state)
        self.assertNotIn('score',state)

    def test_timed_sql_failure_cannot_be_a_speedup(self):
        failure = subprocess.CompletedProcess(['mysql'],1,stdout='',stderr='ERROR 4012: query timeout')
        with patch.object(demo.subprocess,'run',return_value=failure):
            with self.assertRaisesRegex(RuntimeError,'query timeout'):
                demo.sql('select 1',timed=True)

    def test_partial_batch_results_are_rejected(self):
        for call in (demo.cpu_lateral,demo.gpu_batch):
            with self.subTest(backend=call.__name__):
                with patch.object(demo,'sql',return_value=(0.5,0)),patch.object(demo,'sql_rows',return_value=[['999']]):
                    with self.assertRaisesRegex(RuntimeError,'Incomplete batch'):
                        call(100,k=10)

    def test_successful_batch_keeps_the_measured_sql_time(self):
        for call in (demo.cpu_lateral,demo.gpu_batch):
            with self.subTest(backend=call.__name__):
                with patch.object(demo,'sql',return_value=(0.5,0)),patch.object(demo,'sql_rows',return_value=[['1000']]):
                    self.assertEqual(call(100,k=10),0.5)


if __name__=='__main__':
    unittest.main()
