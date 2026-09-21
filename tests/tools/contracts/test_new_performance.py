import copy
import importlib.util
from pathlib import Path
import sys
import unittest
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("new_experiment_stats", ROOT/"tools/performance/summarize_portfolio_experiment.py")
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

def raw(cpu=10, gpu=.2, resident=100):
    samples=[{"cpuFrameMs":float(cpu),"gpuFrameMs":float(gpu),"residentBytes":resident} for _ in range(600)]
    return {"samples":samples,"correctness":{"status":"PASS","validationMessages":0},
            "statistics":{key:m.distribution([s[key] for s in samples]) for key in samples[0]}}

def runs(values,gpu=.2):
    result=[]
    for i,v in enumerate(values):
        data=raw(v,gpu)
        result.append({"sequence":i+1,"raw":data,"statistics":m.statistics_for(data)})
    return result

class NewExperimentContracts(unittest.TestCase):
    def test_gpu_missing_values_not_zero_measurements(self):
        data=raw()
        for sample in data["samples"][-3:]:sample["gpuFrameMs"]=0
        data["statistics"]["gpuFrameMs"]=m.distribution([s["gpuFrameMs"] for s in data["samples"]])
        result=m.statistics_for(data)
        self.assertEqual(result["gpuValidCount"],597)
        self.assertEqual(result["gpuValid"]["median"],.2)

    def test_gpu_coverage_rejected(self):
        data=raw()
        for sample in data["samples"][-7:]:sample["gpuFrameMs"]=0
        data["statistics"]["gpuFrameMs"]=m.distribution([s["gpuFrameMs"] for s in data["samples"]])
        with self.assertRaisesRegex(ValueError,"coverage"):m.statistics_for(data)

    def test_stored_statistic_tamper(self):
        data=raw();data["statistics"]["cpuFrameMs"]["median"]=9
        with self.assertRaisesRegex(ValueError,"differ"):m.statistics_for(data)

    def test_missing_frame(self):
        data=raw();data["samples"].pop()
        with self.assertRaises(ValueError):m.statistics_for(data)

    def test_validation_failure(self):
        data=raw();data["correctness"]["validationMessages"]=1
        with self.assertRaises(ValueError):m.statistics_for(data)

    def test_nonfinite(self):
        with self.assertRaises(ValueError):m.distribution([1,float("inf")])

    def test_improvement(self):
        self.assertEqual(m.compare(runs([10]*5),runs([8]*5))["result"],"ACCEPTED")

    def test_gpu_regression_blocks_cpu_improvement(self):
        self.assertEqual(m.compare(runs([10]*5),runs([8]*5,gpu=.3))["result"],"REJECTED")

    def test_drift_takes_precedence(self):
        self.assertEqual(m.compare(runs([10,10,10,11,11]),runs([8]*5))["result"],"INCONCLUSIVE")

    def test_chronology_not_input_list_order(self):
        data=runs([10,10,10,11,11])
        self.assertAlmostEqual(m.drift(list(reversed(data))),10)

    def test_no_improvement_rejected(self):
        self.assertEqual(m.compare(runs([10]*5),runs([10]*5))["result"],"REJECTED")

    def test_zero_baseline_hitch_rate_has_absolute_margin(self):
        a,b=runs([10]*5),runs([8]*5)
        for run in b:
            for sample in run["raw"]["samples"][:7]:sample["cpuFrameMs"]=31
            run["raw"]["statistics"]["cpuFrameMs"]=m.distribution([s["cpuFrameMs"] for s in run["raw"]["samples"]])
            run["statistics"]=m.statistics_for(run["raw"])
        result=m.compare(a,b)
        self.assertFalse(result["protected"]["hitch"]["pass"])

if __name__=="__main__":unittest.main()

