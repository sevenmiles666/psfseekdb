import importlib.util
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch


root = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('batch_demo',root/'notebooks/seekdb-hipvs-batch-demo/lib/seekdb_demo.py')
demo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(demo)


class BatchDemoTest(unittest.TestCase):
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
