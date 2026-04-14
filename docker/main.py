from fastapi import FastAPI, HTTPException
from pydantic import BaseModel, ValidationError
from deepeval.metrics import GEval
from deepeval.test_case import LLMTestCase, LLMTestCaseParams
from typing import Optional

app = FastAPI()

class TestData(BaseModel):
    input: str
    actual_output: str
    expected_output: Optional[str] = None 
    context: Optional[str] = None
    retrieval_context: Optional[list[str]] = None
    evaluation_criteria: Optional[list[str]] = None

@app.post("/evaluate/")
async def evaluate_llm_response(data: TestData):
    try:
        user_fields = data.dict(exclude_none=True).keys()
        evaluation_params = [
            param
            for param in LLMTestCaseParams
            if param.value in user_fields
        ]

        correctness_metric = GEval(
            name="Correctness",
            evaluation_steps=data.evaluation_criteria or [
                "Check whether 'actual output' has all the files or code snippets (Controller, service, entity, repository) from 'expected output'",
                "The entity class import statements should use 'jakarta.persistence' instead of 'javax.persistence'",
                "Check if the correct dependencies are added in the 'pom.xml' file",
            ],
            evaluation_params=evaluation_params,
            threshold=0.8
        )

        test_case = LLMTestCase(
            input=data.input,
            actual_output=data.actual_output,
            expected_output=data.expected_output,
            context=data.context,
            retrieval_context=data.retrieval_context
        )

        correctness_metric.measure(test_case)
        # assert_test(test_case, [correctness_metric])

        result = {
            "score": correctness_metric.score,
            "reason": correctness_metric.reason
        }

        return result
        
    except ValidationError as ve:
        # Handle validation errors specifically from Pydantic models
        raise HTTPException(status_code=422, detail=str(ve))
    except KeyError as ke:
        # Handle missing keys or incorrect field names
        raise HTTPException(status_code=400, detail=f"Key Error: {str(ke)}")
    except AttributeError as ae:
        # Handle missing attributes or methods within the evaluation logic
        raise HTTPException(status_code=500, detail=f"Attribute Error: {str(ae)}")
    except Exception as e:
        # A generic error handler for any other type of exception
        raise HTTPException(status_code=500, detail=f"An unexpected error occurred: {str(e)}")


