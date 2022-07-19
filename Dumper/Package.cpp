#include "Package.h"
#include "Settings.h"
#include <algorithm>


void Package::Process(std::vector<int32_t>& PackageMembers)
{
	for (int32_t Index : PackageMembers)
	{
		UEObject Object = ObjectArray::GetByIndex(Index);

		if (!Object || Object.HasAnyFlags(EObjectFlags::RF_ClassDefaultObject))
		{
			continue;
		}

		if (Object.IsA(EClassCastFlags::UEnum))
		{
			GenerateEnum(Object.Cast<UEEnum&>());
		}
		else if (Object.IsA(EClassCastFlags::UClass))
		{
			GenerateClass(Object.Cast<UEClass&>());
		}
		else if (Object.IsA(EClassCastFlags::UStruct) && !Object.IsA(EClassCastFlags::UFunction))
		{
			GenerateStruct(Object.Cast<UEStruct&>());
		}
	}
}

void Package::GenerateMembers(std::vector<UEProperty>& MemberVector, UEStruct& Super, Types::Class& Class)
{
	int PrevPropertyEnd = Super.GetSuper() ? Super.GetSuper().GetStructSize() : 0;
	int PrevBoolPropertyEnd = 0;
	int PrevBoolPropertyBit = 1;

	for (auto& Property : MemberVector)
	{
		std::string CppType = Property.GetCppType();
		std::string Name = Property.GetValidName();

		const int Offset = Property.GetOffset();
		const int Size = Property.GetSize();

		std::string Comment = std::format("0x{:X}(0x{:X})({})", Offset, Size, Property.StringifyFlags());

		if (Offset > PrevPropertyEnd)
		{
			Class.AddMember(GenerateBytePadding(PrevPropertyEnd, Offset - PrevPropertyEnd, "Fixing Size after Last Property  [ Dumper-7 ]"));
		}

		if (Property.IsA(EClassCastFlags::UBoolProperty) &&  !Property.Cast<UEBoolProperty>().IsNativeBool())
		{
			Name += " : 1";

			const uint8 BitIndex = Property.Cast<UEBoolProperty>().GetBitIndex();

			Comment = std::format("Mask : 0x{:X} {}", Property.Cast<UEBoolProperty>().GetFieldMask(), Comment);

			if (PrevBoolPropertyEnd < Offset)
				PrevBoolPropertyBit = 1;

			if (PrevBoolPropertyBit < BitIndex)
			{
				Class.AddMember(GenerateBitPadding(Offset, BitIndex - PrevBoolPropertyBit, "Fixing Bit-Field Size  [ Dumper-7 ]"));
			}

			PrevBoolPropertyBit = BitIndex + 1;
			PrevBoolPropertyEnd = Offset;
		}

		Types::Member Member(CppType, Name, Comment);

		PrevPropertyEnd = Offset + Size;

		Class.AddMember(Member);
	}
}

Types::Function Package::GenerateFunction(UEFunction& Function, UEStruct& Super)
{
	std::string ReturnType = "void";
	std::vector<Types::Parameter> Params;
	std::string FuncBody;

	std::vector<std::string> OutPtrParamNames;

	bool bHasRetType = false;
	
	for (UEProperty Param = Function.GetChild().Cast<UEProperty>(); Param; Param = Param.GetNext().Cast<UEProperty>())
	{
		bool bIsRef = false;
		bool bIsOut = false;
		bool bIsConst = false;

		std::string Type = Param.GetCppType();

		if (Param.HasPropertyFlags(EPropertyFlags::ConstParm))
		{
			Type = "const " + Type;
			bIsConst = true;
		}
		if (Param.HasPropertyFlags(EPropertyFlags::ReferenceParm))
		{
			Type += "&";
			bIsRef = true;
			bIsOut = true;
		}
		if (Param.HasPropertyFlags(EPropertyFlags::OutParm) && !bIsRef)
		{
			Type += "*";
			bIsOut = true;
			OutPtrParamNames.push_back(Param.GetValidName());
		}

		if (!bIsOut && !bIsRef && (Param.IsA(EClassCastFlags::UArrayProperty) || Param.IsA(EClassCastFlags::UStrProperty)))
		{
			Type += "&";
			
			if(!bIsConst)
				Type = "const " + Type;
		}

		if (Param.HasPropertyFlags(EPropertyFlags::ReturnParm))
		{
			ReturnType = Type;
			bHasRetType = true;
		}
		else
		{
			Params.push_back(Types::Parameter(Type, Param.GetValidName(), bIsOut && !bIsRef));
		}
	}
	
	Types::Function Func(ReturnType, Function.GetValidName(), Params, false);

	FuncBody += std::format("static auto Func = UObject::GetFunction(\"{}\", \"{}\");\n\n", Super.GetName(), Function.GetName());

	if (Settings::bUseNamespaceForParams)
		FuncBody += "Params::";

	FuncBody += std::format("{} Parms;\n", Function.GetParamStructName());

	for (auto& Param : Func.GetParameters())
	{
		FuncBody += std::format("Parms.{0} = {0};\n", Param.GetName());
	}

	if(Function.HasFlags(EFunctionFlags::Native))
		FuncBody += "\nauto Flags = Func->FunctionFlgas;\nFunc->FunctionFlgas |= 0x400;\n\n";

	FuncBody += "UObject::ProcessEvent(Func, &Parms);";

    if (Function.HasFlags(EFunctionFlags::Native))
        FuncBody += "\n\nFunc->FunctionFlgas = Flags;\n\n";


	for (auto& Name : OutPtrParamNames)
	{
		FuncBody += std::format("if ({0} != nullptr)\n\t{0} = Parms.{0}", Name);
	}

	if (bHasRetType)
		FuncBody += "\n\nreturn Parms.ReturnValue;\n";

	Func.AddBody(FuncBody);

	return Func;
}

Types::Struct Package::GenerateStruct(UEStruct& Struct, bool bIsFunction)
{
}

Types::Class Package::GenerateClass(UEClass& Class)
{
	std::string ClassName = Class.GetCppName();

	Types::Class RetClass = Types::Class(ClassName);

	int Size = Class.GetStructSize();
	int SuperSize = 0;

	if (UEStruct Super = Class.GetSuper())
	{
		RetClass = Types::Class(ClassName, Super.GetCppName());
		SuperSize = Super.GetStructSize();
	}

	RetClass.AddComment(std::format("0x{:X} (0x{:X} - 0x{:X})", Size - SuperSize, Size, SuperSize));
	RetClass.AddComment(Class.GetFullName());

	std::vector<UEProperty> Properties;

	static int NumProps = 0;
	static int NumFuncs = 0;

	for (UEField Child = Class.GetChild(); Child; Child = Child.GetNext())
	{
		if (Child.IsA(EClassCastFlags::UProperty))
		{
			Properties.push_back(Child.Cast<UEProperty>());
		}
		else if (Child.IsA(EClassCastFlags::UFunction))
		{
			RetClass.AddFunction(GenerateFunction(Child.Cast<UEFunction&>(), Class));
		}
	}

	std::sort(Properties.begin(), Properties.end(), [](UEProperty Left, UEProperty Right) -> bool
		{
			if (Left.IsA(EClassCastFlags::UBoolProperty) && Right.IsA(EClassCastFlags::UBoolProperty))
			{
				if (Left.GetOffset() == Right.GetOffset())
				{
					return Left.Cast<UEBoolProperty>().GetFieldMask() < Right.Cast<UEBoolProperty>().GetFieldMask();
				}
			}

			return Left.GetOffset() < Right.GetOffset();
		});

	GenerateMembers(Properties, Class, RetClass);

	return RetClass;
}

Types::Enum Package::GenerateEnum(UEEnum& Enum)
{
}

Types::Member Package::GenerateBytePadding(int32 Offset, int32 PadSize, std::string&& Reason)
{
	static uint32 PadNum = 0;

	return Types::Member("uint8", std::format("Pad_{:X}[0x{:X}]", PadNum++, PadSize), Reason);
}

Types::Member Package::GenerateBitPadding(int32 Offset, int32 PadSize, std::string&& Reason)
{
	return Types::Member("uint8", std::format(": {:X}", PadSize), Reason);
}